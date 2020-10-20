# p4sync
This is a simple p4 command line wrapper that aids in automatic p4 syncs.  It supports multi-threaded syncs.

NOTE: It is up to the user to correctly configure P4's environment.

# Syntax
```sh
p4sync [-threads=THREAD_COUNT] [DEPOT_PATH ...]
```

If no arguments are given, the entire depot is synced.