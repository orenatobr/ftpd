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

#pragma once

#include <cstdio>
#include <string_view>

#ifdef __3DS__
/// \brief 3DS virtual root: "/" is the SD card plus folders for the internal NAND partitions
namespace vroot
{
/// \brief Register the virtual root device and mount the NAND archives
bool init ();

/// \brief Unmount the NAND archives and unregister the virtual root device
void exit ();

/// \brief Whether a top-level folder (a NAND device) is mounted
/// \param name_ Folder name (e.g. "ctrnand")
bool mounted (std::string_view name_);

/// \brief Whether a folder is a HOME save archive (opt-in, writes need a commit)
/// \param name_ Folder name (e.g. "homesys")
bool isSave (std::string_view name_);

/// \brief Open a file of the HOME extdata for writing (modes "r+" and "w" only)
/// \param path_ Native path (e.g. "homeext:/SaveData.dat")
/// \param mode_ fopen mode
/// \return The stream, or nullptr if the path is not in a mounted extdata
std::FILE *openExt (char const *path_, char const *mode_);

/// \brief Cut and commit an extdata file opened with openExt, after a clean transfer
/// \param stream_ Stream returned by openExt, not closed yet
/// \return Whether the changes were committed (true for a stream that is not an extdata file)
bool extFinish (std::FILE *stream_);

/// \brief Commit pending writes to one mounted HOME save archive
/// \param name_ Folder name (e.g. "homesys")
/// \return Whether the changes were committed
bool commit (std::string_view name_);
}
#endif
