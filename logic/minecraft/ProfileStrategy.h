#pragma once

#include "ProfileUtils.h"

class MinecraftProfile;

class ProfileStrategy
{
	friend class MinecraftProfile;
public:
	virtual ~ProfileStrategy(){};

	/// load the patch files into the profile
	virtual void load() = 0;

	/// save the order of patches, given the order
	virtual bool saveOrder(ProfileUtils::PatchOrder order) = 0;

	/// install a list of jar mods into the instance
	virtual bool installJarMods(QStringList filepaths) = 0;

	/// remove any files or records that constitute the version patch
	virtual bool removePatch(PackagePtr jarMod) = 0;

protected:
	MinecraftProfile *profile;
};
