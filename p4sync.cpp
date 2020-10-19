//-----------------------------------------------------------------------------
// Copyright (C) Doppelgamer LLC, 2020
// All rights reserved.
//-----------------------------------------------------------------------------
#define _CRT_SECURE_NO_WARNINGS
#include <cassert>
#include <cstdarg>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef min
#undef max

using namespace std;
using namespace std::literals;
using namespace std::literals::string_view_literals;
using cstr = const char*;
template <class T> auto operator*(const T& s) { return s.c_str(); }

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
   if (vsnprintf_s(buf, sizeof buf, fmt, args) < 0)
      throw exception("Invalid format");
   va_end(args);

   SetConsoleTextAttribute(console, color);
   OutputDebugStringA(buf);
   printf("%s", buf);
   SetConsoleTextAttribute(console, white);
}

string get_temp_path()
{
   char path[L_tmpnam];
   if (!tmpnam(path))
      throw exception("Failed to generate temporary file path");
   return path;
}

const string starting_dir = "//metr/Game/Main/"s;
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

vector<string> needs_resolving;
mutex needs_resolving_mu;

void add_needs_resolving(const string& file)
{
   lock_guard<mutex> guard(needs_resolving_mu);
   needs_resolving.push_back(file);
}

atomic<bool> critical = false;
atomic<int> errors = 0;
atomic<int> clobbered = 0;
atomic<int> updated = 0;
atomic<int> added = 0;
atomic<int> deleted = 0;

const unordered_map<string, tuple<string, int, atomic<int>*>> result_map
{
   {"updating",   {"updated", green,   &updated}},
   {"added as",   {"added",   cyan,    &added}},
   {"deleted as", {"deleted", blue,    &deleted}},
};

struct connection_error : exception {};

bool is_connection_error(const string& output)
{
   static constexpr cstr connection_errors[]
   {
      "Connect to server failed; check $P4PORT.",
      "Your session has expired, please login again.",
      "Perforce password (P4PASSWD) invalid or unset.",
      "RpcTransport: partial message read",
      "TCP receive failed.",
      "read: socket: WSAECONNRESET",
   };

   // This could probably be more efficient, but keep it simple for now
   for (cstr e: connection_errors)
      if (output.find(e) != string::npos)
         return true;

   return false;
}

template <class... Ts>
string p4cmd(Ts&&... args)
{
   char buf[1024];
   auto len = sprintf_s(buf, args...);
   assert(len >= 0);
   string temp_path = get_temp_path();
   sprintf_s(buf+len, sizeof buf-len, " > %s", *temp_path);
   if (system(buf) == -1)
      throw exception("Invalid system command");
   string output = load_file(*temp_path);
   if (is_connection_error(output))
   {
      critical = true;
      throw connection_error();
   }
   return output;
}

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
         dirs_to_sync.push_back(starting_dir + argv[i]);
   }

   if (dirs_to_sync.empty())
      dirs_to_sync.emplace_back(starting_dir);

   print(white, "Starting sync with %d threads\n", num_threads);
   for (string dir: dirs_to_sync) // iterating by value intentionally
   {
      while (dir.back() == '/' || dir.back() == '.' || dir.back() == '*')
         dir.pop_back();

      print(white, "Syncing %s\n", *dir);
      string file = p4cmd("p4 sync -n \"%s/...\"", *dir);
      char* p = file.data();
      while (*p)
      {
         cstr left = p;
         while (*p && *p != '#') p++;
         if (!*p)
            throw exception("Error while parsing tosync list");
         *p++ = 0;
         work.emplace_back(left);
         p += strcspn(p, "\r\n");
         p += strspn(p, "\r\n\t ");
      }

      work_total = (int)work.size();
      work_index = 0;
   }

   vector<thread> workers;
   for (int i=0; i<num_threads; i++)
   {
      workers.emplace_back([](int id)
      {
         while (!work_done() && !critical.load())
         {
            if (string path; try_get_work(path))
            {
               string output = p4cmd("p4 -s sync \"%s#head\"", *path);
               auto [first_line, rest] = split(output, "\n"sv);

               //if (begins_with_nocase(*rest, "error: "))
               //{
               //   auto [prefix, body] = split(first_line, ": "sv);

               //   if (begins_with_nocase(*body, "Can't clobber writable file"))
               //   {
               //      clobbered++;
               //      print(yellow, "Clobbering file: %s...\n", *path);
               //      p4cmd("p4 sync -f \"%s\"", *path);                  
               //   }
               //   else if (body.find("must resolve #head"))
               //   {
               //      add_needs_resolving(path);
               //      print(red, "%s\n", *body);
               //   }
               //   else
               //   {
               //      errors++;
               //      print(red, "%s\n", *body);
               //   }
               //}
               if (output.find("Can't clobber writable file") != string::npos)
               {
                  clobbered++;
                  print(yellow, "clobbered %s\n", *path);
                  p4cmd("p4 sync -f \"%s\"", *path);
               }
               else if (output.find("must resolve #head") != string::npos)
               {
                  add_needs_resolving(path);
                  //print(red, "%s\n", *body);
                  print(red, "conflict: %s\n", *path);
               }
               else if (size_t ierror=output.find("error: "); ierror!=string::npos)
               {
                  errors++;
                  print(red, "%s\n", *output.substr(ierror));
               }
               else
               {
                  auto [left, right] = split(first_line, " - "s);

                  for (const auto& i: result_map)
                  {
                     if (begins_with_nocase(*right, *i.first))
                     {
                        auto [prefix, color, var] = i.second;
                        print(color, "%-7s %s\n", *prefix, *path);
                        (*var)++;
                        break;
                     }
                  }
               }
            }
            else this_thread::sleep_for(2ms);
         }
      }, i);
   }

   while (!work_done() && !critical.load())
      this_thread::sleep_for(100ms);

   for (thread& worker: workers)
      worker.join();

   auto color = errors > 0 ? red : clobbered > 0 || !needs_resolving.empty() ? yellow : white;
   print(color, "Sync finished\n  Errors: %d\n  Conflicts: %d\n  Clobbered: %d\n  Updated: %d\n  Added: %d\n  Deleted: %d\n",
      errors.load(), (int)needs_resolving.size(), clobbered.load(), updated.load(), added.load(), deleted.load());
   if (!needs_resolving.empty())
   {
      print(color, "Files need resolving:\n");
      for (const auto& file: needs_resolving)
         print(color, "  %s\n", *file);
   }
   return 0;
}
