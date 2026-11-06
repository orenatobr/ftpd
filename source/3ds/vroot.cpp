// ftpd is a server implementation based on the following:
// - RFC  959 (https://tools.ietf.org/html/rfc959)
// - RFC 3659 (https://tools.ietf.org/html/rfc3659)
// - suggested implementation details from https://cr.yp.to/ftp/filesystem.html
// - Deflate transmission mode for FTP
//   (https://tools.ietf.org/html/draft-preston-ftpext-deflate-04)
//
// Copyright (C) 2026 Michael Theall
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "vroot.h"

#include <3ds.h>

#include <dirent.h>
#include <sys/iosupport.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

namespace
{
/// \brief Archive to mount as a top-level directory of the virtual root
struct Mount
{
	/// \brief Device name (also the directory name seen by FTP clients)
	char const *name;
	/// \brief Archive to mount
	FS_ArchiveID id;
	/// \brief Binary archive path (media type and save data id), unused when dataSize is 0
	std::array<u32, 3> data;
	/// \brief Size in bytes of the binary archive path, 0 for an empty path
	u32 dataSize;
	/// \brief Whether the mount shows up as a folder in "/" (the SD card is the root itself)
	bool folder;
	/// \brief HOME save: mounted only when R is held at launch, and writes need an explicit commit
	bool save;
	/// \brief Whether the archive was mounted successfully
	bool mounted;
};

/// \brief Mounts exposed under "/"
std::array<Mount, 6> s_mounts{{
    {"sdmc", ARCHIVE_SDMC, {}, 0, false, false, false},
    {"ctrnand", ARCHIVE_NAND_CTR_FS, {}, 0, true, false, false},
    {"twln", ARCHIVE_NAND_TWL_FS, {}, 0, true, false, false},
    {"twlp", ARCHIVE_TWL_PHOTO, {}, 0, true, false, false},
    // HOME Menu (EUR) layout saves, opened through the system so the CMAC stays valid:
    // Launcher.dat (system save 0x00020098) and SaveData.dat (SD extdata 0x00000098)
    {"homesys", ARCHIVE_SYSTEM_SAVEDATA, {MEDIATYPE_NAND, 0x00020098, 0}, 8, true, true, false},
    {"homeext", ARCHIVE_EXTDATA, {MEDIATYPE_SD, 0x00000098, 0}, 12, true, true, false},
}};

/// \brief Directory iterator state for the virtual root
struct RootDir
{
	/// \brief Index of the next mount to return
	std::size_t index;
	/// \brief Iterator over the SD card, whose entries follow the mount folders
	DIR *sd;
};

/// \brief Virtual root device
devoptab_t s_devoptab{};

/// \brief Whether a path handed to the devoptab names the virtual root
/// \note The path may or may not carry the "vroot:" device prefix
bool isRoot (char const *path_)
{
	if (auto const colon = std::strchr (path_, ':'))
		path_ = colon + 1;

	return path_[0] == '\0' || std::strcmp (path_, "/") == 0;
}

int rootStat (_reent *const r_, char const *const path_, struct stat *const st_)
{
	// only "/" exists
	if (!isRoot (path_))
	{
		r_->_errno = ENOENT;
		return -1;
	}

	std::memset (st_, 0, sizeof (*st_));
	st_->st_mode  = S_IFDIR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
	st_->st_nlink = 1;
	return 0;
}

int rootErrno (_reent *const r_, int const err_)
{
	r_->_errno = err_;
	return -1;
}

int rootChdir (_reent *const r_, char const *const path_)
{
	if (!isRoot (path_))
		return rootErrno (r_, ENOENT);

	return 0;
}

DIR_ITER *rootDirOpen (_reent *const r_, DIR_ITER *const dir_, char const *const path_)
{
	if (!isRoot (path_))
	{
		r_->_errno = ENOENT;
		return nullptr;
	}

	auto const state = static_cast<RootDir *> (dir_->dirStruct);
	state->index     = 0;
	state->sd        = ::opendir ("sdmc:/");
	if (!state->sd)
	{
		r_->_errno = errno;
		return nullptr;
	}

	return dir_;
}

int rootDirReset (_reent *const, DIR_ITER *const dir_)
{
	auto const state = static_cast<RootDir *> (dir_->dirStruct);
	state->index     = 0;
	::rewinddir (state->sd);
	return 0;
}

int rootDirNext (_reent *const r_, DIR_ITER *const dir_, char *const name_, struct stat *const st_)
{
	auto const state = static_cast<RootDir *> (dir_->dirStruct);

	// libctru hands us a buffer the size of dirent::d_name
	constexpr auto nameSize = sizeof (dirent::d_name);

	// first the mount folders (/ctrnand, ...)
	while (state->index < s_mounts.size () &&
	       !(s_mounts[state->index].folder && s_mounts[state->index].mounted))
		++state->index;

	if (state->index < s_mounts.size ())
	{
		std::strncpy (name_, s_mounts[state->index++].name, nameSize - 1);
		name_[nameSize - 1] = '\0';
		return rootStat (r_, "/", st_);
	}

	// then the SD card contents, hiding anything that a mount folder shadows
	for (;;)
	{
		auto const entry = ::readdir (state->sd);
		if (!entry)
			return rootErrno (r_, ENOENT);

		if (vroot::mounted (entry->d_name))
			continue;

		std::strncpy (name_, entry->d_name, nameSize - 1);
		name_[nameSize - 1] = '\0';

		std::string const path = std::string ("sdmc:/") + entry->d_name;
		if (::stat (path.c_str (), st_) != 0)
			std::memset (st_, 0, sizeof (*st_));
		return 0;
	}
}

int rootDirClose (_reent *const, DIR_ITER *const dir_)
{
	auto const state = static_cast<RootDir *> (dir_->dirStruct);
	if (state->sd)
		::closedir (state->sd);
	state->sd = nullptr;
	return 0;
}

// the root is read-only: every mutating operation fails with EROFS
int rootOpen (_reent *const r_, void *, char const *const, int const, int const)
{
	return rootErrno (r_, EROFS);
}

int rootUnlink (_reent *const r_, char const *const)
{
	return rootErrno (r_, EROFS);
}

int rootRename (_reent *const r_, char const *const, char const *const)
{
	return rootErrno (r_, EROFS);
}

int rootMkdir (_reent *const r_, char const *const, int const)
{
	return rootErrno (r_, EROFS);
}
}

bool vroot::init ()
{
	std::memset (&s_devoptab, 0, sizeof (s_devoptab));
	s_devoptab.name        = "vroot";
	s_devoptab.structSize  = 1;
	s_devoptab.open_r      = rootOpen;
	s_devoptab.unlink_r    = rootUnlink;
	s_devoptab.rename_r    = rootRename;
	s_devoptab.mkdir_r     = rootMkdir;
	s_devoptab.rmdir_r     = rootUnlink;
	s_devoptab.stat_r      = rootStat;
	s_devoptab.lstat_r     = rootStat;
	s_devoptab.chdir_r     = rootChdir;
	s_devoptab.dirStateSize = sizeof (RootDir);
	s_devoptab.diropen_r   = rootDirOpen;
	s_devoptab.dirreset_r  = rootDirReset;
	s_devoptab.dirnext_r   = rootDirNext;
	s_devoptab.dirclose_r  = rootDirClose;

	if (AddDevice (&s_devoptab) < 0)
		return false;

	// libctru already mounts "sdmc"; the others are optional since a process
	// without the matching filesystem access rights is refused by fs:USER
	s_mounts[0].mounted = true;

	// the HOME saves are writable over anonymous FTP, so they are opt-in: hold R while launching
	hidScanInput ();
	auto const saves = (hidKeysHeld () & KEY_R) != 0;

	for (std::size_t i = 1; i < s_mounts.size (); ++i)
	{
		auto &mount = s_mounts[i];
		if (mount.save && !saves)
			continue;

		auto const path = mount.dataSize == 0
		                      ? fsMakePath (PATH_EMPTY, "")
		                      : FS_Path{PATH_BINARY, mount.dataSize, mount.data.data ()};
		mount.mounted = R_SUCCEEDED (archiveMount (mount.id, path, mount.name));
	}

	return true;
}

void vroot::exit ()
{
	// uncommitted writes are dropped on unmount on purpose: they come from failed transfers
	for (std::size_t i = 1; i < s_mounts.size (); ++i)
	{
		if (s_mounts[i].mounted)
			archiveUnmount (s_mounts[i].name);
		s_mounts[i].mounted = false;
	}

	RemoveDevice ("vroot:");
}

bool vroot::mounted (std::string_view const name_)
{
	for (auto const &mount : s_mounts)
	{
		if (mount.folder && mount.mounted && name_ == mount.name)
			return true;
	}

	return false;
}

bool vroot::isSave (std::string_view const name_)
{
	for (auto const &mount : s_mounts)
	{
		if (mount.save && name_ == mount.name)
			return true;
	}

	return false;
}

void vroot::commit (std::string_view const name_)
{
	// save data archives drop uncommitted writes when they are unmounted
	for (auto const &mount : s_mounts)
	{
		if (mount.save && mount.mounted && name_ == mount.name)
			archiveCommitSaveData (mount.name);
	}
}
