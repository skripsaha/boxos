#ifndef TAGFS_RESERVED_H
#define TAGFS_RESERVED_H
#define TAGFS_RESERVED_KEYS(X) \
    X("system") X("utility") X("app") X("god") X("stopped") X("bypass") \
    X("network") X("trashed") X("hidden") X("autostart") X("snapshot") X("name")
#define TAGFS_RESERVED_COUNT 12

#define TAGFS_AUTH_KEYS(X) \
    X(SYSTEM,"system") X(UTILITY,"utility") X(APP,"app") X(GOD,"god") \
    X(STOPPED,"stopped") X(BYPASS,"bypass") X(NETWORK,"network")
#define TAGFS_AUTH_COUNT 7
#endif