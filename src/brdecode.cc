// A quick-and-dirty tool to convert print files back to pbm images.
//
// Copyright 2013 Peter De Wachter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <algorithm>
#include <exception>
#include <vector>
#include <string>
#include <functional>

namespace {

const size_t MAX_LINE_SIZE = 2000;

FILE *in_file;
std::vector<std::vector<uint8_t>> page;
std::vector<uint8_t> line;
size_t line_offset;


class unsupported_compression: public std::exception {
 private:
  char msg[128];
 public:
  unsupported_compression(unsigned int type) {
    sprintf(msg, "Unsupported raster compression type %d", type);
  }
  virtual const char *what() const noexcept {
    return msg;
  }
};

class read_past_block_end: public std::exception {
 public:
  virtual const char *what() const noexcept {
    return "Attempt to read data past end of block";
  }
};

class unexpected_eof: public std::exception {
 public:
  virtual const char *what() const noexcept {
    return "Unexpected EOF";
  }
};

class line_overflow: public std::exception {
 public:
  virtual const char *what() const noexcept {
    return "Unreasonable long line, aborting";
  }
};


uint8_t get() {
  int ch = getc(in_file);
  if (ch < 0)
    throw unexpected_eof();
  return ch;
}

unsigned read_overflow(std::function<uint8_t()> next_character) {
  uint8_t ch;
  unsigned sum = 0;
  do {
    ch = next_character();
    sum += ch;
  } while (ch == 255);
  return sum;
}

void read_repeat(uint8_t cmd, std::function<uint8_t()> next_character) {
  uint16_t offset = (cmd >> 5) & 3;
  if (offset == 3)
    offset += read_overflow(next_character);
  uint16_t count = cmd & 31;
  if (count == 31)
    count += read_overflow(next_character);
  count += 2;
  uint8_t data = next_character();

  size_t end = line_offset + offset + count;
  if (end > line.size()) {
    if (end > MAX_LINE_SIZE)
      throw line_overflow();
    line.resize(end);
  }
  line_offset += offset;
  std::fill_n(line.begin() + line_offset, count, data);
  line_offset += count;
}

void read_substitute(uint8_t cmd, std::function<uint8_t()> next_character) {
  uint16_t offset = (cmd >> 3) & 15;
  if (offset == 15)
    offset += read_overflow(next_character);
  uint16_t count = cmd & 7;
  if (count == 7)
    count += read_overflow(next_character);
  count += 1;

  size_t end = line_offset + offset + count;
  if (end > line.size()) {
    if (end > MAX_LINE_SIZE)
      throw line_overflow();
    line.resize(end);
  }
  line_offset += offset;
  std::generate_n(line.begin() + line_offset, count, next_character);
  line_offset += count;
}

void read_edit(std::function<uint8_t()> next_character) {
  int8_t cmd = next_character();
  if (cmd < 0) {
    read_repeat(cmd, next_character);
  } else {
    read_substitute(cmd, next_character);
  }
}

void read_line(std::function<uint8_t()> next_character) {
  uint8_t num_edits = next_character();
  if (num_edits == 255) {
    line.clear();
  } else {
    line_offset = 0;
    for (int i = 0; i < num_edits; ++i) {
      read_edit(next_character);
    }
  }
  page.push_back(line);
}

void read_block(std::function<uint8_t()> next_character) {
  unsigned count = next_character();
  count = count * 256 + next_character();
  for (unsigned i = 0; i < count; ++i) {
    read_line(next_character);
  }
}

bool read_page() {
  bool in_raster = false;
  unsigned number, digit;
  int format = 0;
  int ch, ch1, ch2;

  page.clear();
  line.clear();
  while ((ch = getc(in_file)) >= 0) {
    if (ch == '\f') {
      break;
    } else if (ch == 033) {
      ch1 = get();
      ch2 = get();
      if (ch1 == '*' && ch2 == 'b') {
        // start of PCL raster escape sequence
        in_raster = true;
        number = 0;
      }
    } else if (in_raster) {
      if (ch >= '0' && ch <= '9') {
        // parse value field
        digit = ch - '0';
        number = number * 10 + digit;
      } else if (ch == 'm' || ch == 'M') {
        // format parameter
        format = number;
      } else if (ch == 'w' || ch == 'W') {
        // graphics data
        auto get_next_block_character = [&]() {
          if (number <= 0)
            throw read_past_block_end();
          number--;
          return get();
        };
        if (format == 1030) {
          read_block(get_next_block_character);
        } else {
          throw unsupported_compression(format);
        }
        if (number > 0) {
          fprintf(stderr, "WARNING: %d unread bytes in block\n", number);
          for (; number > 0; number--) {
            get();
          }
        }
      }
      if (ch >= '`' && ch <= '~') {
        // lowercase -> parameter character
        number = 0;
      } else if (ch >= '@' && ch <= '^') {
        // uppercase -> terminating character
        in_raster = false;
      }
    }
  }
  return !page.empty();
}

void write_pnm(FILE *f) {
  size_t height = page.size();
  size_t width = 0;
  for (auto &l : page) {
    width = std::max(width, l.size());
  }

  fprintf(f, "P4 %zd %zd\n", width * 8, height);
  std::vector<uint8_t> empty(width);
  for (auto &l : page) {
    fwrite(l.data(), 1, l.size(), f);
    fwrite(empty.data(), 1, width - l.size(), f);
  }
}

}  // namespace

int main(int argc, char *argv[]) {
  const char *in_filename;
  std::string out_prefix;

  if (argc > 2) {
    in_filename = argv[1];
    out_prefix = argv[2];
  } else if (argc > 1) {
    in_filename = argv[1];
    out_prefix = argv[1];
  } else {
    in_filename = nullptr;
    out_prefix = "page";
  }

  if (in_filename) {
    in_file = fopen(in_filename, "rb");
    if (!in_file) {
      fprintf(stderr, "Can't open file \"%s\"\n", in_filename);
      return 1;
    }
  } else {
    in_file = stdin;
    if (isatty(0)) {
      fprintf(stderr, "No filename given and no input on stdin\n");
      return 1;
    }
  }

  try {
    int page_num = 1;
    while (read_page()) {
      std::string out_filename = out_prefix
        + "-" + std::to_string(page_num) + ".pbm";
      FILE *out_file = fopen(out_filename.c_str(), "wb");
      if (!out_file) {
        fprintf(stderr, "Can't write file \"%s\"\n", out_filename.c_str());
        return 1;
      }
      write_pnm(out_file);
      fclose(out_file);
      fprintf(stderr, "%s\n", out_filename.c_str());
      ++page_num;
    }
  } catch (std::exception &e) {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return 0;
}
