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

#pragma once

#include <QAbstractListModel>

#include <QString>
#include <QList>
#include <memory>

#include "RawLibrary.h"
#include "VersionFile.h"
#include "JarMod.h"

class ProfileStrategy;
class OneSixInstance;

class MinecraftProfile : public QAbstractListModel
{
	Q_OBJECT
	friend class ProfileStrategy;

public:
	explicit MinecraftProfile(ProfileStrategy *strategy);

	/// construct a MinecraftProfile from a single file
	static std::shared_ptr<MinecraftProfile> fromJson(const QJsonObject &obj);

	void setStrategy(ProfileStrategy * strategy);
	ProfileStrategy *strategy();

	virtual QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const;
	virtual QVariant headerData(int section, Qt::Orientation orientation, int role) const;
	virtual int rowCount(const QModelIndex &parent = QModelIndex()) const;
	virtual int columnCount(const QModelIndex &parent) const;
	virtual Qt::ItemFlags flags(const QModelIndex &index) const;

	/// install more jar mods
	void installJarMods(QStringList selectedFiles);

	/// Can patch file # be removed?
	bool canRemove(const int index) const;

	enum MoveDirection { MoveUp, MoveDown };
	/// move patch file # up or down the list
	void move(const int index, const MoveDirection direction);

	/// remove patch file # - including files/records
	bool remove(const int index);

	/// remove patch file by id - including files/records
	bool remove(const QString id);

	/// reload all profile patches from storage, clear the profile and apply the patches
	void reload();

	/// clear the profile
	void clear();

	/// apply the patches
	void reapply();

	/// do a finalization step (should always be done after applying all patches to profile)
	void finalize();

public:
	/// get all java libraries that belong to the classpath
	QList<RawLibraryPtr> getActiveNormalLibs();

	/// get all native libraries that need to be available to the process
	QList<RawLibraryPtr> getActiveNativeLibs();

	/// get file ID of the patch file at #
	QString versionFileId(const int index) const;

	/// get the profile patch by id
	ProfilePatchPtr versionPatch(const QString &id);

	/// get the profile patch by index
	ProfilePatchPtr versionPatch(int index);

	/// save the current patch order
	void saveCurrentOrder() const;

public: /* only use in ProfileStrategy */
	/// Remove all the patches
	void clearPatches();

	/// Add the patch object to the internal list of patches
	void appendPatch(ProfilePatchPtr patch);

public: /* data */
	/// Assets type - "legacy" or a version ID
	QString assets;
	/**
	 * arguments that should be used for launching minecraft
	 *
	 * ex: "--username ${auth_player_name} --session ${auth_session}
	 *      --version ${version_name} --gameDir ${game_directory} --assetsDir ${game_assets}"
	 */
	QString minecraftArguments;
	/**
	 * A list of all tweaker classes
	 */
	QStringList tweakers;
	/**
	 * The main class to load first
	 */
	QString mainClass;
	/**
	 * The applet class, for some very old minecraft releases
	 */
	QString appletClass;

	/// the list of libs - both active and inactive, native and java
	QList<RawLibraryPtr> libraries;

	/// traits, collected from all the version files (version files can only add)
	QSet<QString> traits;

	/// A list of jar mods. version files can add those.
	QList<JarmodPtr> jarMods;

private:
	QList<ProfilePatchPtr> VersionPatches;
	ProfileStrategy *m_strategy = nullptr;
};
