//-----------------------------------------------------------------------------
// Copyright (C) Doppelgamer LLC, 2020
// All rights reserved.
//-----------------------------------------------------------------------------
#define _CRT_SECURE_NO_WARNINGS
#include <cassert>
#include <vector>
#include <string>
#include <cstdarg>
#include <future>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef min
#undef max

#define BATCH_SYNC 0

using namespace std;
using namespace std::literals;
using namespace std::literals::string_view_literals;

using cstr = const char*;

template <class T> auto operator*(const T& s) { return s.c_str(); }

static constexpr int KB = 1024;
static constexpr int MB = KB * 1024;
static constexpr int GB = MB * 1024;

bool begins_with(cstr s, cstr p)
{
   for (; *s && *p && *s==*p; s++, p++);
   return *p == 0;
}

bool begins_with_nocase(cstr s, cstr p)
{
   for (; *s && *p && tolower(*s)==tolower(*p); s++, p++);
   return *p == 0;
}

pair<string, string> split(const string& s, const string_view& delim)
{
   size_t mid = s.find(delim);
   return {s.substr(0, mid), s.substr(mid + delim.size())};
}

string load_file(cstr path)
{
   FILE* file = nullptr;
   auto ret = fopen_s(&file, path, "rb");
   if (ret != 0 || !file)
      throw exception(("Failed to load file: "s + path).c_str());
   fseek(file, 0L, SEEK_END);
   size_t len = ftell(file);
   fseek(file, 0L, SEEK_SET);
   string s(len, ' ');
   fread(s.data(), 1, len, file);
   fclose(file);
   return s;
}

enum
{
   black, navy, forest, teal, maroon, purple, ochre, silver,
   gray, blue, green, cyan, red, magenta, yellow, white,
};

mutex print_mu;

void print(int color, cstr fmt, ...)
{
   lock_guard<mutex> guard(print_mu);
   static thread_local HANDLE console = nullptr;
   if (!console) console = GetStdHandle(STD_OUTPUT_HANDLE);
   static char buf[1024];

   va_list args;
   va_start(args, fmt);
   if (vsnprintf(buf, sizeof buf, fmt, args) < 0)
      throw exception("Invalid format");
   va_end(args);

   SetConsoleTextAttribute(console, color);
   OutputDebugStringA(buf);
   printf("%s", buf);
   SetConsoleTextAttribute(console, white);
}

template <class... Ts>
void sysfmt(Ts&&... args)
{
   char buf[1024];
   sprintf_s(buf, args...);
   if (system(buf) == -1)
      throw exception("call to system function failed");
}

vector<string> get_lines(const string& path)
{
   string file = load_file(*path);
   vector<string> lines;
   auto start = begin(file);
   for (auto i=start; i!=end(file);)
   {
      if ((*i == '\n' || *i == '\r') && i != start)
      {
         lines.emplace_back(start, i);
         while (i != end(file) && (*i == '\n' || *i == '\r'))
            ++i;
         start = i;
      }
      else ++i;
   }
   return lines;
}

string get_temp_path()
{
   char path[L_tmpnam];
   if (!tmpnam(path))
      throw exception("Failed to generate temporary file path");
   return path;
}

//-----------------------------------------------------------------------------
const tuple<string, int, bool> msg_types[]
{
   {"error", red, true},
   {"warning", yellow, true},
   {"info", green, false},
   {"info1", green, false},
   {"info2", green, false},
};

const string starting_dir = "//metr/Game/Main/"s;
const int64_t batch_size = 50 * MB;

vector<string> work;
atomic<int> work_total = 0;
atomic<int> work_index = 0;
mutex work_mu;

bool work_done()
{
   lock_guard<mutex> guard(work_mu);
   return work.empty();
}

bool try_get_work(string& task)
{
   lock_guard<mutex> guard(work_mu);
   if (work.empty()) return false;
   task = string(work.back());
   work_index++;
   work.pop_back();
   return true;
}

vector<string> cant_clobber;
mutex cant_clobber_mu;

void add_cant_clobber(const string& path)
{
   lock_guard<mutex> guard(cant_clobber_mu);
   cant_clobber.push_back(path);
}

atomic<bool> critical = false;
atomic<int> errors = 0;
atomic<bool> done = false;

static cstr connection_errors[]
{
   "Connect to server failed; check $P4PORT.",
   "Your session has expired, please login again.",
   "Perforce password (P4PASSWD) invalid or unset.",
   "RpcTransport: partial message read",
   "TCP receive failed.",
   "read: socket: WSAECONNRESET",
};

int main(int argc, char* argv[])
{
   int num_threads = 8;
   vector<string> dirs_to_sync;

   for (int i=1; i<argc; i++)
   {
      string threads_tag = "-threads="s;
      if (begins_with_nocase(argv[i], *threads_tag))
      {
         string num = argv[i] + threads_tag.size();
         try { num_threads = stoi(num); }
         catch (const exception& e) { print(red, "Invalid value for num threads argument: %s\n", e.what()); }
      }
      else
      {
         dirs_to_sync.push_back(starting_dir + argv[i]);
      }
   }

   if (dirs_to_sync.empty())
      dirs_to_sync.emplace_back(starting_dir);

   print(white, "Starting sync with %d threads\n", num_threads);
   for (string dir: dirs_to_sync) // iterating by value intentionally
   {
      while (dir.back() == '/' || dir.back() == '.' || dir.back() == '*')
         dir.pop_back();
      print(white, "Syncing %s\n", *dir);
      string temp_path = get_temp_path();
      sysfmt("p4 sync -n \"%s/...\" > %s", *dir, *temp_path);
      string file = load_file(*temp_path);
      char* p = file.data();
      while (*p)
      {
         cstr left = p;
         while (*p && *p != '#') p++;
         if (!*p) throw exception("Error while parsing tosync list");
         *p++ = 0;
         work.emplace_back(left);
         p += strcspn(p, "\r\n");
         p += strspn(p, "\r\n\t ");
      }

      work_total = (int)work.size();
      work_index = 0;
   #endif
   }

   vector<thread> workers;
   for (int i=0; i<num_threads; i++)
   {
      workers.emplace_back([](int id)
      {
         string cmd;
         while (!done.load() && !critical.load())
         {
            if (string path; try_get_work(path))
            {
               //print(gray, "[%d/%d]: Syncing %s...\n", work_index.load(), work_total.load(), *path);
               string temp_path = get_temp_path();
               sysfmt("p4 -s sync \"%s#head\" > %s", *path, *temp_path);
               bool clobber = false;
               bool error = false;
               for (const auto& line: get_lines(temp_path))
               {
                  auto [prefix, body] = split(line, ": "sv);
                  for (const auto& [tag, color, iserror]: msg_types)
                  {
                     if (prefix == tag)
                     {
                        print(color, "%s\n", *body);
                        if (iserror)
                        {
                           error = true;
                           for (const auto& e: connection_errors)
                              if (line.find(e) != string::npos)
                                 critical = true;

                           if (begins_with_nocase(*body, "Can't clobber writable file"))
                              clobber = true;
                        }
                        break;
                     }
                  }
               }

               if (error)
                  errors++;

               if (clobber)
               {
                  print(yellow, "Clobbering file: %s...\n", *path);
                  sysfmt("p4 sync -f \"%s\" > %s", *path, *temp_path);
               }
            }
            else this_thread::sleep_for(2ms);
         }
      }, i);
   }

   while (!work_done() && !critical.load())
   {
      this_thread::sleep_for(100ms);
   }

   done = true;
   for (thread& worker: workers)
      worker.join();

   int numerrors = errors.load();
   print(white, "Sync finished: %d errors\n", numerrors);
   return 0;
}
