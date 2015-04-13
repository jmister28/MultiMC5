/* Copyright 2013-2015 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Env.h"
#include "OneSixUpdate.h"

#include <QtNetwork>

#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDataStream>
#include <pathutils.h>
#include <JlCompress.h>

#include "BaseInstance.h"
#include "MetaPackageList.h"
#include "minecraft/MinecraftProfile.h"
#include "minecraft/Library.h"
#include "minecraft/onesix/OneSixInstance.h"
#include "net/URLConstants.h"
#include "MMCZip.h"
#include <tasks/SequentialTask.h>

OneSixUpdate::OneSixUpdate(OneSixInstance *inst, QObject *parent) : Task(parent), m_inst(inst)
{
}

void OneSixUpdate::executeTask()
{
	// Make directories
	QDir mcDir(m_inst->minecraftRoot());
	if (!mcDir.exists() && !mcDir.mkpath("."))
	{
		emitFailed(tr("Failed to create folder for minecraft binaries."));
		return;
	}

	bool updateTask = false;
	versionUpdateTask = std::make_shared<SequentialTask>();

	auto addTasklet = [&](const QString & uid, const QString & version)
	{
		if(!version.isEmpty())
		{
			auto list = std::dynamic_pointer_cast<MetaPackageList>(ENV.getVersionList(uid));
			versionUpdateTask->addTask(std::shared_ptr<Task>(list->getLoadTask()));
			versionUpdateTask->addTask(list->createUpdateTask(version));
			updateTask = true;
		}
	};

	addTasklet("net.minecraft", m_inst->minecraftVersion());
	addTasklet("org.lwjgl", m_inst->lwjglVersion());
	addTasklet("com.mumfrey.liteloader", m_inst->liteloaderVersion());
	addTasklet("net.minecraftforge", m_inst->forgeVersion());

	if (!updateTask)
	{
		qDebug() << "Didn't spawn an update task.";
		jarlibStart();
		return;
	}
	connect(versionUpdateTask.get(), SIGNAL(succeeded()), SLOT(jarlibStart()));
	connect(versionUpdateTask.get(), SIGNAL(failed(QString)), SLOT(versionUpdateFailed(QString)));
	connect(versionUpdateTask.get(), SIGNAL(progress(qint64, qint64)),
			SIGNAL(progress(qint64, qint64)));
	setStatus(tr("Getting the version files from Mojang..."));
	versionUpdateTask->start();
}

void OneSixUpdate::versionUpdateFailed(QString reason)
{
	emitFailed(reason);
}

void OneSixUpdate::jarlibStart()
{
	setStatus(tr("Getting the library files from Mojang..."));
	qDebug() << m_inst->name() << ": downloading libraries";
	OneSixInstance *inst = (OneSixInstance *)m_inst;
	try
	{
		inst->reloadProfile();
	}
	catch (MMCError &e)
	{
		emitFailed(e.cause());
		return;
	}
	catch (...)
	{
		emitFailed(tr("Failed to load the version description file for reasons unknown."));
		return;
	}

	auto job = new NetJob(tr("Libraries for instance %1").arg(inst->name()));
	jarlibDownloadJob.reset(job);

	// Build a list of URLs that will need to be downloaded.
	std::shared_ptr<MinecraftProfile> version = inst->getMinecraftProfile();

	auto libs = version->resources.getActiveNativeLibs();
	libs.append(version->resources.getActiveNormalLibs());

	auto metacache = ENV.metacache();
	QList<LibraryPtr> brokenLocalLibs;

	for (auto lib : libs)
	{
		if (lib->hint() == "local")
		{
			if (!lib->filesExist(m_inst->librariesPath()))
				brokenLocalLibs.append(lib);
			continue;
		}

		QString raw_storage = lib->storagePath();
		QString raw_dl = lib->downloadUrl();

		auto f = [&](QString storage, QString dl)
		{
			auto entry = metacache->resolveEntry("libraries", storage);
			if (entry->stale)
			{
				jarlibDownloadJob->addNetAction(CacheDownload::make(dl, entry));
			}
		};
		if (raw_storage.contains("${arch}"))
		{
			QString cooked_storage = raw_storage;
			QString cooked_dl = raw_dl;
			f(cooked_storage.replace("${arch}", "32"), cooked_dl.replace("${arch}", "32"));
			cooked_storage = raw_storage;
			cooked_dl = raw_dl;
			f(cooked_storage.replace("${arch}", "64"), cooked_dl.replace("${arch}", "64"));
		}
		else
		{
			f(raw_storage, raw_dl);
		}
	}
	if (!brokenLocalLibs.empty())
	{
		jarlibDownloadJob.reset();
		QStringList failed;
		for (auto brokenLib : brokenLocalLibs)
		{
			failed.append(brokenLib->files());
		}
		QString failed_all = failed.join("\n");
		emitFailed(tr("Some libraries marked as 'local' are missing their jar "
					  "files:\n%1\n\nYou'll have to correct this problem manually. If this is "
					  "an externally tracked instance, make sure to run it at least once "
					  "outside of MultiMC.").arg(failed_all));
		return;
	}

	connect(jarlibDownloadJob.get(), SIGNAL(succeeded()), SLOT(jarlibFinished()));
	connect(jarlibDownloadJob.get(), SIGNAL(failed()), SLOT(jarlibFailed()));
	connect(jarlibDownloadJob.get(), SIGNAL(progress(qint64, qint64)),
			SIGNAL(progress(qint64, qint64)));

	jarlibDownloadJob->start();
}

void OneSixUpdate::jarlibFinished()
{
	OneSixInstance *inst = (OneSixInstance *)m_inst;
	std::shared_ptr<MinecraftProfile> version = inst->getMinecraftProfile();

	// create temporary modded jar, if needed
	QList<Mod> jarMods;
	for (auto jarmod : version->resources.jarMods)
	{
		QString filePath = inst->jarmodsPath().absoluteFilePath(jarmod->name);
		jarMods.push_back(Mod(QFileInfo(filePath)));
	}
	if(jarMods.size())
	{
		auto finalJarPath = QDir(m_inst->instanceRoot()).absoluteFilePath("temp.jar");
		QFile finalJar(finalJarPath);
		if(finalJar.exists())
		{
			if(!finalJar.remove())
			{
				emitFailed(tr("Couldn't remove stale jar file: %1").arg(finalJarPath));
				return;
			}
		}
		auto libs = version->resources.getActiveNormalLibs();
		QString sourceJarPath;

		// find net.minecraft:minecraft
		for(auto foo:libs)
		{
			// FIXME: stupid hardcoded thing
			if(foo->artifactPrefix() == "net.minecraft:minecraft")
			{
				sourceJarPath = m_inst->librariesPath().absoluteFilePath( foo->storagePath());
				break;
			}
		}

		// use it as the source for jar modding
		if(!MMCZip::createModdedJar(sourceJarPath, finalJarPath, jarMods))
		{
			emitFailed(tr("Failed to create the custom Minecraft jar file."));
			return;
		}
	}
	if (version->resources.traits.contains("legacyFML"))
	{
		fmllibsStart();
	}
	else
	{
		assetIndexStart();
	}
}

void OneSixUpdate::jarlibFailed()
{
	QStringList failed = jarlibDownloadJob->getFailedFiles();
	QString failed_all = failed.join("\n");
	emitFailed(
		tr("Failed to download the following files:\n%1\n\nPlease try again.").arg(failed_all));
}

void OneSixUpdate::fmllibsStart()
{
	// Get the mod list
	OneSixInstance *inst = (OneSixInstance *)m_inst;
	std::shared_ptr<MinecraftProfile> fullversion = inst->getMinecraftProfile();
	bool forge_present = false;

	QString version = inst->minecraftVersion();
	auto &fmlLibsMapping = g_VersionFilterData.fmlLibsMapping;
	if (!fmlLibsMapping.contains(version))
	{
		assetIndexStart();
		return;
	}

	auto &libList = fmlLibsMapping[version];

	// determine if we need some libs for FML or forge
	setStatus(tr("Checking for FML libraries..."));
	forge_present = (fullversion->versionPatch("net.minecraftforge") != nullptr);
	// we don't...
	if (!forge_present)
	{
		assetIndexStart();
		return;
	}

	// now check the lib folder inside the instance for files.
	for (auto &lib : libList)
	{
		QFileInfo libInfo(PathCombine(inst->libDir(), lib.filename));
		if (libInfo.exists())
			continue;
		fmlLibsToProcess.append(lib);
	}

	// if everything is in place, there's nothing to do here...
	if (fmlLibsToProcess.isEmpty())
	{
		assetIndexStart();
		return;
	}

	// download missing libs to our place
	setStatus(tr("Dowloading FML libraries..."));
	auto dljob = new NetJob("FML libraries");
	auto metacache = ENV.metacache();
	for (auto &lib : fmlLibsToProcess)
	{
		auto entry = metacache->resolveEntry("fmllibs", lib.filename);
		QString urlString = lib.ours ? URLConstants::FMLLIBS_OUR_BASE_URL + lib.filename
									 : URLConstants::FMLLIBS_FORGE_BASE_URL + lib.filename;
		dljob->addNetAction(CacheDownload::make(QUrl(urlString), entry));
	}

	connect(dljob, SIGNAL(succeeded()), SLOT(fmllibsFinished()));
	connect(dljob, SIGNAL(failed()), SLOT(fmllibsFailed()));
	connect(dljob, SIGNAL(progress(qint64, qint64)), SIGNAL(progress(qint64, qint64)));
	legacyDownloadJob.reset(dljob);
	legacyDownloadJob->start();
}

void OneSixUpdate::fmllibsFinished()
{
	legacyDownloadJob.reset();
	if (!fmlLibsToProcess.isEmpty())
	{
		setStatus(tr("Copying FML libraries into the instance..."));
		OneSixInstance *inst = (OneSixInstance *)m_inst;
		auto metacache = ENV.metacache();
		int index = 0;
		for (auto &lib : fmlLibsToProcess)
		{
			progress(index, fmlLibsToProcess.size());
			auto entry = metacache->resolveEntry("fmllibs", lib.filename);
			auto path = PathCombine(inst->libDir(), lib.filename);
			if (!ensureFilePathExists(path))
			{
				emitFailed(tr("Failed creating FML library folder inside the instance."));
				return;
			}
			if (!QFile::copy(entry->getFullPath(), PathCombine(inst->libDir(), lib.filename)))
			{
				emitFailed(tr("Failed copying Forge/FML library: %1.").arg(lib.filename));
				return;
			}
			index++;
		}
		progress(index, fmlLibsToProcess.size());
	}
	assetIndexStart();
}

void OneSixUpdate::fmllibsFailed()
{
	emitFailed("Game update failed: it was impossible to fetch the required FML libraries.");
	return;
}

