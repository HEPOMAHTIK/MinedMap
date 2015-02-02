/*
  Copyright (c) 2015, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "World/Region.hpp"
#include "NBT/ListTag.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <system_error>

#include <sys/types.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <png.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>


using namespace MinedMap;


static const size_t DIM = World::Region::SIZE*World::Chunk::SIZE;


static void addChunk(uint32_t image[DIM][DIM], size_t X, size_t Z, const World::Chunk *chunk) {
	World::Chunk::Blocks layer = chunk->getTopLayer();

	for (size_t x = 0; x < World::Chunk::SIZE; x++) {
		for (size_t z = 0; z < World::Chunk::SIZE; z++)
			image[Z*World::Chunk::SIZE+z][X*World::Chunk::SIZE+x] = htonl(layer.blocks[x][z].getColor());
	}
}

static void writePNG(const char *filename, const uint32_t data[DIM][DIM]) {
	std::FILE *f = std::fopen(filename, "wb");
	if (!f)
		throw std::system_error(errno, std::generic_category(), "unable to open output file");

	png_structp png_ptr = png_create_write_struct (PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png_ptr)
		throw std::runtime_error("unable to create PNG write struct");

	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		png_destroy_write_struct(&png_ptr, nullptr);
		throw std::runtime_error("unable to create PNG info struct");
	}

	if (setjmp(png_jmpbuf(png_ptr))) {
		png_destroy_write_struct(&png_ptr, &info_ptr);
		fclose(f);
		throw std::runtime_error("unable to write PNG file");
	}

	png_init_io(png_ptr, f);

	png_set_IHDR(png_ptr, info_ptr, DIM, DIM, 8, PNG_COLOR_TYPE_RGB_ALPHA,
		     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

	uint8_t *row_pointers[World::Region::SIZE*World::Chunk::SIZE];
	for (size_t i = 0; i < World::Region::SIZE*World::Chunk::SIZE; i++)
		row_pointers[i] = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(data[i]));

	png_set_rows(png_ptr, info_ptr, row_pointers);
	png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

	std::fclose(f);
}

static void doRegion(const std::string &input, const std::string &output) {
	struct stat instat, outstat;

	if (stat(input.c_str(), &instat) < 0) {
		std::fprintf(stderr, "Unable to stat %s: %s\n", input.c_str(), std::strerror(errno));
		return;
	}

	if (stat(output.c_str(), &outstat) == 0) {
		if (instat.st_mtim.tv_sec < outstat.st_mtim.tv_sec ||
		    (instat.st_mtim.tv_sec == outstat.st_mtim.tv_sec && instat.st_mtim.tv_nsec <= outstat.st_mtim.tv_nsec)) {
			    std::fprintf(stderr, "%s is up-to-date.\n", output.c_str());
			    return;
		    }
	}

	std::fprintf(stderr, "Generating %s from %s...\n", output.c_str(), input.c_str());

	const std::string tmpfile = output + ".tmp";

	try {
		uint32_t image[DIM][DIM] = {};
		World::Region::visitChunks(input.c_str(), [&image] (size_t X, size_t Z, const World::Chunk *chunk) { addChunk(image, X, Z, chunk); });

		writePNG(tmpfile.c_str(), image);

		struct timespec times[2] = {instat.st_mtim, instat.st_mtim};
		if (utimensat(AT_FDCWD, tmpfile.c_str(), times, 0) < 0)
			std::fprintf(stderr, "Warning: failed to set utime on %s: %s\n", tmpfile.c_str(), std::strerror(errno));

		if (std::rename(tmpfile.c_str(), output.c_str()) < 0) {
			std::fprintf(stderr, "Unable to save %s: %s\n", output.c_str(), std::strerror(errno));
			unlink(tmpfile.c_str());
		}
	}
	catch (const std::exception& ex) {
		std::fprintf(stderr, "Failed to generate %s: %s\n", output.c_str(), ex.what());
		unlink(tmpfile.c_str());
	}
}

int main(int argc, char *argv[]) {
	if (argc < 3) {
		std::fprintf(stderr, "Usage: %s <data directory> <output directory>\n", argv[0]);
		return 1;
	}

	std::string inputdir(argv[1]);
	inputdir += "/region";

	std::string outputdir(argv[2]);

	DIR *dir = opendir(inputdir.c_str());
	if (!dir) {
		std::fprintf(stderr, "Unable to read input directory: %s\n", std::strerror(errno));
		return 1;
	}

	int minX = INT_MAX, maxX = INT_MIN, minZ = INT_MAX, maxZ = INT_MIN;

	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr) {
		int x, z;
		if (std::sscanf(entry->d_name, "r.%i.%i.mca", &x, &z) == 2) {
			size_t l = strlen(entry->d_name) + 1;
			char buf[l];
			std::snprintf(buf, l, "r.%i.%i.mca", x, z);
			if (std::memcmp(entry->d_name, buf, l))
				continue;

			if (x < minX)
				minX = x;
			if (x > maxX)
				maxX = x;

			if (z < minZ)
				minZ = z;
			if (z > maxZ)
				maxZ = z;

			std::string name(entry->d_name);

			doRegion(inputdir + "/" + name, outputdir + "/" + name.substr(0, name.length()-3) + "png");
		}
	}

	closedir(dir);

	return 0;
}
