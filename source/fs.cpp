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

#include "fs.h"
#include "ioBuffer.h"
#include "vroot.h"

#include <gsl/pointers>
#include <gsl/util>

#include <array>
#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__NDS__) || defined(__3DS__) || defined(__SWITCH__)
#define getline __getline
#endif

std::string fs::printSize (std::uint64_t const size_)
{
	constexpr std::uint64_t const KiB = 1024;
	constexpr std::uint64_t const MiB = 1024 * KiB;
	constexpr std::uint64_t const GiB = 1024 * MiB;
	constexpr std::uint64_t const TiB = 1024 * GiB;
	constexpr std::uint64_t const PiB = 1024 * TiB;
	constexpr std::uint64_t const EiB = 1024 * PiB;

	std::array<char, 64> buffer{};

	for (auto const &[name, bin] : {
	         // clang-format off
	         std::make_pair ("EiB", EiB),
	         std::make_pair ("PiB", PiB),
	         std::make_pair ("TiB", TiB),
	         std::make_pair ("GiB", GiB),
	         std::make_pair ("MiB", MiB),
	         std::make_pair ("KiB", KiB)
	         // clang-format on
	     })
	{
		// get the integral portion of the number
		auto const whole = size_ / bin;
		if (size_ >= 100 * bin)
		{
			// >= 100, print xxxXiB
			std::size_t const size = std::sprintf (buffer.data (), "%" PRIu64 "%s", whole, name);
			return {buffer.data (), size};
		}

		// get the fractional portion of the number
		auto const frac = size_ - (whole * bin);
		if (size_ >= 10 * bin)
		{
			// >= 10, print xx.xXiB
			std::size_t const size = std::sprintf (
			    buffer.data (), "%" PRIu64 ".%" PRIu64 "%s", whole, frac * 10 / bin, name);
			return {buffer.data (), size};
		}

		if (size_ >= 1000 * (bin / KiB))
		{
			// >= 1000 of lesser bin, print x.xxXiB
			std::size_t const size = std::sprintf (
			    buffer.data (), "%" PRIu64 ".%02" PRIu64 "%s", whole, frac * 100 / bin, name);
			return {buffer.data (), size};
		}
	}

	// < 1KiB, just print the number
	std::size_t const size = std::sprintf (buffer.data (), "%" PRIu64 "B", size_);
	return {buffer.data (), size};
}

///////////////////////////////////////////////////////////////////////////
fs::File::~File ()
{
	std::free (m_lineBuffer);
}

fs::File::File () = default;

fs::File::File (File &&that_) = default;

fs::File &fs::File::operator= (File &&that_) = default;

fs::File::operator bool () const
{
	return static_cast<bool> (m_fp);
}

fs::File::operator FILE * () const
{
	return m_fp.get ();
}

void fs::File::setBufferSize (std::size_t const size_)
{
	if (m_buffer.size () != size_)
		m_buffer.resize (size_);

	if (m_fp)
		(void)std::setvbuf (m_fp.get (), m_buffer.data (), _IOFBF, m_buffer.size ());
}

std::string fs::native (std::string_view const path_)
{
#ifdef __3DS__
	// paths that already carry a device prefix (romfs:/, sdmc:/) are not virtual
	if (path_.empty () || path_.front () != '/')
		return std::string (path_);

	// the virtual root lists the SD card and the NAND folders
	if (path_.size () == 1)
		return "vroot:/";

	auto const end = path_.find ('/', 1);
	auto const dev = path_.substr (1, end == std::string_view::npos ? end : end - 1);

	// NAND folders map to their own device, everything else lives on the SD card
	if (vroot::mounted (dev))
		return std::string (dev) + ':' + (end == std::string_view::npos ? "/" : std::string (path_.substr (end)));

	return "sdmc:" + std::string (path_);
#else
	return std::string (path_);
#endif
}

bool fs::File::open (gsl::not_null<char const *> const path_,
    gsl::not_null<char const *> const mode_)
{
	auto const native = fs::native (path_.get ());
	gsl::owner<FILE *> fp = nullptr;
#ifdef __3DS__
	// the generic archive devoptab refuses writes to extdata files, so go through the FS API
	if (std::strpbrk (mode_.get (), "wa+"))
		fp = vroot::openExt (native.c_str (), mode_);
#endif
	if (!fp)
		fp = std::fopen (native.c_str (), mode_);
	if (!fp)
		return false;

#ifdef __3DS__
	// only writers need a commit, and only for the archive they write to
	auto const dev = native.substr (0, native.find (':'));
	m_save.clear ();
	if (vroot::isSave (dev) && std::strpbrk (mode_.get (), "wa+"))
		m_save = dev;
#endif

	m_fp = std::unique_ptr<std::FILE, int (*) (std::FILE *)> (fp, &std::fclose);

	if (!m_buffer.empty ())
		(void)std::setvbuf (m_fp.get (), m_buffer.data (), _IOFBF, m_buffer.size ());

	return true;
}

void fs::File::close ()
{
	// a write that failed is not committed, so unmounting drops it
	auto const clean = m_fp && std::ferror (m_fp.get ()) == 0 && std::fflush (m_fp.get ()) == 0;
	m_fp.reset ();

#ifdef __3DS__
	if (clean && !m_save.empty ())
		vroot::commit (m_save);
	m_save.clear ();
#endif
}

int fs::File::seek (std::int64_t const pos_, int const origin_)
{
	static_assert (sizeof (off_t) == 8);
	return ::fseeko (m_fp.get (), pos_, origin_);
}

std::make_signed_t<std::size_t> fs::File::read (gsl::not_null<void *> const buffer_,
    std::size_t const size_)
{
	assert (buffer_);
	assert (size_ > 0);

	auto const rc = std::fread (buffer_, 1, size_, m_fp.get ());
	if (rc == 0)
	{
		if (std::feof (m_fp.get ()))
			return 0;
		return -1;
	}

	return gsl::narrow_cast<std::make_signed_t<std::size_t>> (rc);
}

std::make_signed_t<std::size_t> fs::File::read (IOBuffer &buffer_)
{
	assert (buffer_.freeSize () > 0);

	auto const rc = read (buffer_.freeArea (), buffer_.freeSize ());
	if (rc > 0)
		buffer_.markUsed (rc);

	return rc;
}

std::string_view fs::File::readLine ()
{
	while (true)
	{
		auto rc = ::getline (&m_lineBuffer, &m_lineBufferSize, m_fp.get ());
		if (rc < 0)
			return {};

		while (rc > 0)
		{
			if (m_lineBuffer[rc - 1] != '\r' && m_lineBuffer[rc - 1] != '\n')
				break;

			m_lineBuffer[--rc] = 0;
		}

		if (rc > 0)
			return {m_lineBuffer, gsl::narrow_cast<std::size_t> (rc)};
	}
}

bool fs::File::readAll (gsl::not_null<void *> const buffer_, std::size_t const size_)
{
	assert (buffer_);
	assert (size_ > 0);

	auto const p = static_cast<char *> (buffer_.get ());

	std::size_t bytes = 0;
	while (bytes < size_)
	{
		auto const rc = read (p + bytes, size_ - bytes);
		if (rc <= 0)
			return false;

		bytes += rc;
	}

	return true;
}

std::make_signed_t<std::size_t> fs::File::write (gsl::not_null<void const *> const buffer_,
    std::size_t const size_)
{
	assert (buffer_);
	assert (size_ > 0);

	auto const rc = std::fwrite (buffer_, 1, size_, m_fp.get ());
	if (rc == 0)
		return -1;

	return gsl::narrow_cast<std::make_signed_t<std::size_t>> (rc);
}

std::make_signed_t<std::size_t> fs::File::write (IOBuffer &buffer_)
{
	assert (buffer_.usedSize () > 0);

	auto const rc = write (buffer_.usedArea (), buffer_.usedSize ());
	if (rc > 0)
		buffer_.markFree (rc);

	return rc;
}

bool fs::File::writeAll (gsl::not_null<void const *> const buffer_, std::size_t const size_)
{
	assert (buffer_);
	assert (size_ > 0);

	auto const p = static_cast<char const *> (buffer_.get ());

	std::size_t bytes = 0;
	while (bytes < size_)
	{
		auto const rc = write (p + bytes, size_ - bytes);
		if (rc <= 0)
			return false;

		bytes += rc;
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////
fs::Dir::~Dir () = default;

fs::Dir::Dir () = default;

fs::Dir::Dir (Dir &&that_) = default;

fs::Dir &fs::Dir::operator= (Dir &&that_) = default;

fs::Dir::operator bool () const
{
	return static_cast<bool> (m_dp);
}

fs::Dir::operator DIR * () const
{
	return m_dp.get ();
}

bool fs::Dir::open (gsl::not_null<char const *> const path_)
{
	auto const dp = ::opendir (fs::native (path_.get ()).c_str ());
	if (!dp)
		return false;

	m_dp = std::unique_ptr<DIR, int (*) (DIR *)> (dp, &::closedir);
	return true;
}

void fs::Dir::close ()
{
	m_dp.reset ();
}

dirent *fs::Dir::read ()
{
	errno = 0;
	return ::readdir (m_dp.get ());
}
