#include "QuickModDatabase.h"

#include <QDir>
#include <QCoreApplication>
#include <QTimer>

#include <pathutils.h>

#include "logic/quickmod/InstancePackageList.h"
#include "logic/quickmod/QuickModVersion.h"
#include "logic/quickmod/QuickModMetadata.h"
#include "logic/quickmod/QuickModImagesLoader.h"
#include "logic/quickmod/net/QuickModBaseDownloadAction.h"
#include "logic/MMCJson.h"
#include "logic/OneSixInstance.h"

QuickModDatabase::QuickModDatabase()
	: QObject()
{
	m_dbFile = "quickmods/quickmods.json";
	m_configFile = "quickmods/quickmods.cfg";
	ensureFilePathExists(m_dbFile);

	m_timer.reset(new QTimer(this));
	
	loadFromDisk();
	connect(qApp, &QCoreApplication::aboutToQuit, this, &QuickModDatabase::flushToDisk);
	connect(m_timer.get(), &QTimer::timeout, this, &QuickModDatabase::flushToDisk);
	updateFiles();
}

void QuickModDatabase::setLastETagForURL(const QUrl &url, const QByteArray &ETag)
{
	m_etags[url] = ETag;
	delayedFlushToDisk();
}

// QUESTION replace aboutToReset/reset with specialized signals for repos/indices?
void QuickModDatabase::addRepo(const QString &name, const QUrl &indexUrl)
{
	emit aboutToReset();
	m_indices.insert(name, indexUrl);
	delayedFlushToDisk();
	emit reset();
}

void QuickModDatabase::removeRepo(const QString &name)
{
	emit aboutToReset();
	m_indices.remove(name);
	delayedFlushToDisk();
	emit reset();
}

QByteArray QuickModDatabase::lastETagForURL(const QUrl &url) const
{
	return m_etags[url];
}

void QuickModDatabase::addMod(QuickModMetadataPtr mod)
{
	m_metadata[mod->uid()][mod->repo()] = mod;
	connect(mod->imagesLoader(), &QuickModImagesLoader::iconUpdated, this, &QuickModDatabase::modIconUpdated);
	connect(mod->imagesLoader(), &QuickModImagesLoader::logoUpdated, this, &QuickModDatabase::modLogoUpdated);
	delayedFlushToDisk();
	emit justAddedMod(mod->uid());
}

void QuickModDatabase::addVersion(QuickModVersionPtr version)
{
	// TODO merge versions
	m_versions[version->mod->uid()][version->version().toString()] = version;

	delayedFlushToDisk();
}

QList<QuickModMetadataPtr> QuickModDatabase::allModMetadata(const QuickModRef &uid) const
{
	auto iter = m_metadata.find(uid);
	if (iter == m_metadata.end())
	{
		return {};
	}
	return (*iter).values();
}

QuickModMetadataPtr QuickModDatabase::someModMetadata(const QuickModRef &uid) const
{
	auto iter = m_metadata.find(uid);
	if (iter == m_metadata.end())
	{
		return nullptr;
	}
	if ((*iter).isEmpty())
	{
		return nullptr;
	}
	return *((*iter).begin());
}

QuickModVersionPtr QuickModDatabase::version(const QuickModVersionRef &version) const
{
	for (auto verPtr : m_versions[version.mod()])
	{
		if (verPtr->version() == version)
		{
			return verPtr;
		}
	}
	return QuickModVersionPtr();
}

// TODO fix this
QuickModVersionPtr QuickModDatabase::version(const QString &uid, const QString &version,
											 const QString &repo) const
{
	for (auto verPtr : m_versions[QuickModRef(uid)])
	{
		if (verPtr->versionString == version && verPtr->mod->repo() == repo)
		{
			return verPtr;
		}
	}
	return QuickModVersionPtr();
}

QuickModVersionRef QuickModDatabase::latestVersionForMinecraft(const QuickModRef &modUid,
															   const QString &mcVersion) const
{
	QuickModVersionRef latest;
	for (auto version : versions(modUid, mcVersion))
	{
		if (!latest.isValid())
		{
			latest = version;
		}
		else if (version.isValid() && version > latest)
		{
			latest = version;
		}
	}
	return latest;
}

QStringList QuickModDatabase::minecraftVersions(const QuickModRef &uid) const
{
	QSet<QString> out;
	for (const auto version : m_versions[uid])
	{
		if (version->dependsOnMinecraft())
		{
			out.insert(version->minecraftVersionInterval());
		}
	}
	return out.toList();
}

QList<QuickModVersionRef> QuickModDatabase::versions(const QuickModRef &uid,
													 const QString &mcVersion) const
{
	QSet<QuickModVersionRef> out;
	for (const auto v : m_versions[uid])
	{
		if (v->dependsOnMinecraft() &&
			Util::versionIsInInterval(Util::Version(mcVersion),
									  v->minecraftVersionInterval()))
		{
			out.insert(v->version());
		}
	}
	return out.toList();
}

// FIXME: this doesn't belong here. invert semantics!
QList<QuickModRef>
QuickModDatabase::updatedModsForInstance(std::shared_ptr<OneSixInstance> instance) const
{
	QList<QuickModRef> mods;
	auto iter = instance->installedPackages()->iterateQuickMods();
	while (iter->isValid())
	{
		if (!iter->version().isValid())
		{
			iter->next();
			continue;
		}
		auto latest = latestVersionForMinecraft(iter->uid(), instance->intendedVersionId());
		if (!latest.isValid())
		{
			iter->next();
			continue;
		}
		if (iter->version() < latest)
		{
			mods.append(QuickModRef(iter->uid()));
		}
		iter->next();
	}
	return mods;
}

QString QuickModDatabase::userFacingUid(const QString &uid) const
{
	const auto mod = someModMetadata(QuickModRef(uid));
	return mod ? mod->name() : uid;
}

bool QuickModDatabase::haveUid(const QuickModRef &uid, const QString &repo) const
{
	auto iter = m_metadata.find(uid);
	if (iter == m_metadata.end())
	{
		return false;
	}
	if (repo.isNull())
	{
		return true;
	}
	for (auto mod : *iter)
	{
		if (mod->repo() == repo)
		{
			return true;
		}
	}
	return false;
}

void QuickModDatabase::registerMod(const QString &fileName)
{
	registerMod(QUrl::fromLocalFile(fileName));
}

void QuickModDatabase::registerMod(const QUrl &url)
{
	NetJob *job = new NetJob("QuickMod Download");
	job->addNetAction(QuickModBaseDownloadAction::make(job, url));
	connect(job, &NetJob::succeeded, job, &NetJob::deleteLater);
	connect(job, &NetJob::failed, job, &NetJob::deleteLater);
	job->start();
}

void QuickModDatabase::updateFiles()
{
	NetJob *job = new NetJob("QuickMod Download");
	for (auto mod : m_metadata)
	{
		for (auto meta : mod)
		{
			auto lastETag = lastETagForURL(meta->updateUrl());
			auto download = QuickModBaseDownloadAction::make(job, meta->updateUrl(),
															 meta->uid().toString(), lastETag);
			job->addNetAction(download);
		}
	}
	connect(job, &NetJob::succeeded, job, &NetJob::deleteLater);
	connect(job, &NetJob::failed, job, &NetJob::deleteLater);
	job->start();
}

QList<QuickModRef> QuickModDatabase::getPackageUIDs() const
{
	return m_metadata.keys();
}

void QuickModDatabase::delayedFlushToDisk()
{
	m_isDirty = true;
	m_timer->start(1000); // one second
}

void QuickModDatabase::flushToDisk()
{
	if (!m_isDirty)
	{
		return;
	}

	QJsonObject quickmods;
	for (auto it = m_metadata.constBegin(); it != m_metadata.constEnd(); ++it)
	{
		QJsonObject metadata;
		for (auto metaIt = it.value().constBegin(); metaIt != it.value().constEnd(); ++metaIt)
		{
			metadata[metaIt.key()] = metaIt.value()->toJson();
		}

		auto versionsHash = m_versions[it.key()];
		QJsonObject versions;
		for (auto versionIt = versionsHash.constBegin(); versionIt != versionsHash.constEnd();
			 ++versionIt)
		{
			QJsonObject vObj = versionIt.value()->toJson();
			versions[versionIt.key()] = vObj;
		}

		QJsonObject obj;
		obj.insert("metadata", metadata);
		obj.insert("versions", versions);
		quickmods.insert(it.key().toString(), obj);
	}

	QJsonObject checksums;
	for (auto it = m_etags.constBegin(); it != m_etags.constEnd(); ++it)
	{
		checksums.insert(it.key().toString(), QString::fromLatin1(it.value()));
	}

	QJsonObject indices;
	for (auto it = m_indices.constBegin(); it != m_indices.constEnd(); ++it)
	{
		indices.insert(it.key(), it.value().toString());
	}

	QJsonObject root;
	root.insert("mods", quickmods);
	root.insert("checksums", checksums);
	root.insert("indices", indices);

	if (!ensureFilePathExists(m_dbFile))
	{
		QLOG_ERROR() << "Unable to create folder for QuickMod database:" << m_dbFile;
		return;
	}

	QFile file(m_dbFile);
	if (!file.open(QFile::WriteOnly))
	{
		QLOG_ERROR() << "Unable to save QuickMod Database to disk:" << file.errorString();
		return;
	}
	file.write(QJsonDocument(root).toJson());

	m_isDirty = false;
	m_timer->stop();
}

void QuickModDatabase::loadFromDisk()
{
	using namespace MMCJson;
	try
	{
		emit aboutToReset();
		m_metadata.clear();
		m_versions.clear();
		m_etags.clear();
		m_indices.clear();

		const QJsonObject root =
			ensureObject(MMCJson::parseFile(m_dbFile, "QuickMod Database"));
		const QJsonObject quickmods = ensureObject(root.value("mods"));
		for (auto it = quickmods.constBegin(); it != quickmods.constEnd(); ++it)
		{
			const QString uid = it.key();
			const QJsonObject obj = ensureObject(it.value());

			// metadata
			{
				const QJsonObject metadata = ensureObject(obj.value("metadata"));
				for (auto metaIt = metadata.constBegin(); metaIt != metadata.constEnd();
					 ++metaIt)
				{
					QuickModMetadataPtr ptr = std::make_shared<QuickModMetadata>();
					ptr->parse(MMCJson::ensureObject(metaIt.value()));
					m_metadata[QuickModRef(uid)][metaIt.key()] = ptr;
				}
			}

			// versions
			{
				const QJsonObject versions = ensureObject(obj.value("versions"));
				for (auto versionIt = versions.constBegin(); versionIt != versions.constEnd();
					 ++versionIt)
				{
					// FIXME: giving it a fake 'metadata', because otherwise this causes crashes
					m_versions[QuickModRef(uid)][versionIt.key()] =
						BaseQuickModVersion::parseSingle(
							MMCJson::ensureObject(versionIt.value()),
							*(m_metadata[QuickModRef(uid)].begin()));
				}
			}
		}

		const QJsonObject checksums = ensureObject(root.value("checksums"));
		for (auto it = checksums.constBegin(); it != checksums.constEnd(); ++it)
		{
			m_etags.insert(QUrl(it.key()), ensureString(it.value()).toLatin1());
		}

		const QJsonObject indices = ensureObject(root.value("indices"));
		for (auto it = indices.constBegin(); it != indices.constEnd(); ++it)
		{
			m_indices.insert(it.key(), QUrl(ensureString(it.value())));
		}
	}
	catch (MMCError &e)
	{
		QLOG_ERROR() << "Error while reading QuickMod Database:" << e.cause();
	}
	emit reset();
}
