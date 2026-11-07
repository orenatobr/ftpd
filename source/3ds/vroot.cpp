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
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

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
	/// \brief Whether vroot::suspend unmounted the archive, so vroot::resume must mount it again
	bool suspended = false;
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

/// \brief Log what the filesystem says about opening each HOME save for writing (no data is written)
void probeSaves ()
{
	auto const log = std::fopen ("sdmc:/ftpd-home-diag.txt", "w");
	if (!log)
		return;

	for (auto const &mount : s_mounts)
	{
		if (!mount.save || !mount.mounted)
			continue;

		FS_Archive archive = 0;
		FS_Path const path{PATH_BINARY, mount.dataSize, mount.data.data ()};
		auto const arc = FSUSER_OpenArchive (&archive, mount.id, path);
		std::fprintf (log, "%s: OpenArchive %08lx\n", mount.name, static_cast<unsigned long> (arc));
		if (R_FAILED (arc))
			continue;

		char const *file = mount.id == ARCHIVE_EXTDATA ? "/SaveData.dat" : "/Launcher.dat";
		Handle handle = 0;
		auto const rc = FSUSER_OpenFile (&handle, archive, fsMakePath (PATH_ASCII, file),
		    FS_OPEN_READ | FS_OPEN_WRITE, 0);
		std::fprintf (log, "%s: OpenFile %s read+write %08lx\n", mount.name, file,
		    static_cast<unsigned long> (rc));
		if (R_SUCCEEDED (rc))
			FSFILE_Close (handle);

		FSUSER_CloseArchive (archive);
	}

	std::fclose (log);
}

/// \brief Append the result of a failed extdata finish step to the diagnostics file on the card
void logExtStep (char const *const step_, Result const rc_)
{
	if (auto const log = std::fopen ("sdmc:/ftpd-home-diag.txt", "a"))
	{
		std::fprintf (log, "extFinish: %s %08lx\n", step_, static_cast<unsigned long> (rc_));
		std::fclose (log);
	}
}

/// \brief File of the HOME extdata, opened through the filesystem API directly
/// \note The generic archive devoptab refuses to open extdata files for writing
struct ExtFile
{
	/// \brief Archive handle the file belongs to (also used to commit)
	FS_Archive archive;
	/// \brief File handle
	Handle file;
	/// \brief Current position
	u64 pos;
	/// \brief End of the data written so far
	u64 end;
	/// \brief Whether the file is cut to the written size on close (mode "w")
	bool truncate;
	/// \brief Whether a write failed or came up short, so the file must not be committed
	bool failed;
	/// \brief Stream wrapping this file
	std::FILE *stream;
};

/// \brief Open extdata files by stream, so fs::File can ask for the commit after a clean transfer
std::unordered_map<std::FILE *, ExtFile *> s_extFiles;

int extRead (void *const cookie_, char *const buffer_, std::size_t const size_)
{
	auto const f = static_cast<ExtFile *> (cookie_);
	u32 bytes    = 0;
	if (R_FAILED (FSFILE_Read (f->file, &bytes, f->pos, buffer_, static_cast<u32> (size_))))
	{
		errno = EIO;
		return -1;
	}

	f->pos += bytes;
	return static_cast<int> (bytes);
}

int extWrite (void *const cookie_, char const *const buffer_, std::size_t const size_)
{
	auto const f = static_cast<ExtFile *> (cookie_);
	u32 bytes    = 0;
	if (R_FAILED (FSFILE_Write (f->file, &bytes, f->pos, buffer_, static_cast<u32> (size_), 0)))
	{
		f->failed = true;
		errno     = EIO;
		return -1;
	}

	f->pos += bytes;
	f->end = std::max (f->end, f->pos);
	if (bytes != size_)
	{
		f->failed = true;
		errno     = EIO;
		return -1;
	}

	return static_cast<int> (bytes);
}

fpos_t extSeek (void *const cookie_, fpos_t const offset_, int const whence_)
{
	auto const f = static_cast<ExtFile *> (cookie_);
	u64 size     = 0;
	if (whence_ == SEEK_END && R_FAILED (FSFILE_GetSize (f->file, &size)))
	{
		errno = EIO;
		return -1;
	}

	auto const base = whence_ == SEEK_SET ? 0 : whence_ == SEEK_CUR ? f->pos : size;
	f->pos          = base + offset_;
	return static_cast<fpos_t> (f->pos);
}

int extClose (void *const cookie_)
{
	// closing never truncates or commits: that is decided by vroot::extFinish after a clean
	// transfer, and an unfinished file is dropped with its archive
	auto const f = static_cast<ExtFile *> (cookie_);
	s_extFiles.erase (f->stream);
	FSFILE_Close (f->file);
	FSUSER_CloseArchive (f->archive);
	delete f;
	return 0;
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

	if (saves)
		probeSaves ();

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

void vroot::suspend ()
{
	// HOME needs its own saves while we are suspended: a save archive still open here makes it abort
	// (svcBreak on system save 0x20098). Transfers hold their own archive handle, so while one is
	// open the mounts stay as they are rather than being pulled from under the file.
	if (!s_extFiles.empty ())
		return;

	for (auto &mount : s_mounts)
	{
		if (!mount.save || !mount.mounted)
			continue;

		archiveUnmount (mount.name);
		mount.mounted   = false;
		mount.suspended = true;
	}
}

void vroot::resume ()
{
	for (auto &mount : s_mounts)
	{
		if (!mount.suspended)
			continue;

		FS_Path const path{PATH_BINARY, mount.dataSize, mount.data.data ()};
		mount.mounted   = R_SUCCEEDED (archiveMount (mount.id, path, mount.name));
		mount.suspended = false;
	}
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

bool vroot::commit (std::string_view const name_)
{
	// save data archives drop uncommitted writes when they are unmounted
	for (auto const &mount : s_mounts)
	{
		if (mount.save && mount.mounted && name_ == mount.name)
			return R_SUCCEEDED (archiveCommitSaveData (mount.name));
	}

	return false;
}

std::FILE *vroot::openExt (char const *const path_, char const *const mode_)
{
	std::string_view const path (path_);
	auto const colon = path.find (':');
	if (colon == std::string_view::npos)
		return nullptr;

	for (auto const &mount : s_mounts)
	{
		if (!mount.mounted || mount.id != ARCHIVE_EXTDATA || path.substr (0, colon) != mount.name)
			continue;

		// only "r+" (overwrite in place) and "w" (overwrite, cut to the written size on a clean
		// close) are supported; anything else, append included, is refused
		auto const update = std::strchr (mode_, 'r') && std::strchr (mode_, '+');
		auto const write  = std::strchr (mode_, 'w') != nullptr;
		if (std::strchr (mode_, 'a') || (!update && !write))
		{
			errno = ENOTSUP;
			return nullptr;
		}

		FS_Archive archive = 0;
		FS_Path const archivePath{PATH_BINARY, mount.dataSize, mount.data.data ()};
		if (R_FAILED (FSUSER_OpenArchive (&archive, mount.id, archivePath)))
		{
			errno = EIO;
			return nullptr;
		}

		std::string const file (path.substr (colon + 1));
		Handle handle = 0;
		if (R_FAILED (FSUSER_OpenFile (&handle, archive, fsMakePath (PATH_ASCII, file.c_str ()),
		        FS_OPEN_READ | FS_OPEN_WRITE, 0)))
		{
			FSUSER_CloseArchive (archive);
			errno = ENOENT;
			return nullptr;
		}

		auto const cookie = new ExtFile{archive, handle, 0, 0, write, false, nullptr};
		auto const fp     = ::funopen (cookie, extRead, extWrite, extSeek, extClose);
		if (!fp)
		{
			// nothing was written: just release the handles
			FSFILE_Close (handle);
			FSUSER_CloseArchive (archive);
			delete cookie;
			errno = ENOMEM;
			return nullptr;
		}

		cookie->stream = fp;
		s_extFiles[fp] = cookie;
		return fp;
	}

	return nullptr;
}

bool vroot::extFinish (std::FILE *const stream_)
{
	auto const it = s_extFiles.find (stream_);
	if (it == s_extFiles.end ())
		return true;

	auto const f = it->second;
	if (f->failed)
		return false;

	// mode "w": drop whatever is left of the old content past what was written
	u64 size = 0;
	if (f->truncate)
	{
		if (auto const rc = FSFILE_GetSize (f->file, &size); R_FAILED (rc))
		{
			logExtStep ("GetSize", rc);
			return false;
		}
		if (f->end < size)
		{
			if (auto const rc = FSFILE_SetSize (f->file, f->end); R_FAILED (rc))
			{
				logExtStep ("SetSize", rc);
				return false;
			}
		}
	}

	if (auto const rc = FSFILE_Flush (f->file); R_FAILED (rc))
	{
		logExtStep ("Flush", rc);
		return false;
	}

	// extdata is written through to the card and has no journal to commit (the commit is for save
	// data), so a refusal here does not lose the write: record it and report success
	if (auto const rc =
	        FSUSER_ControlArchive (f->archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, nullptr, 0, nullptr, 0);
	    R_FAILED (rc))
		logExtStep ("ControlArchive commit (ignored)", rc);

	return true;
}
