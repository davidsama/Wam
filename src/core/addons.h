// addons.h - scanning a WoW Interface\AddOns folder.
#ifndef ADDONS_H
#define ADDONS_H

typedef struct WowFlavor { char *label; char *folder; } WowFlavor;
static WowFlavor wow_flavors[] = {
    { "Retail",      "_retail_" },
    { "Classic",     "_classic_" },
    { "Classic Era", "_classic_era_" },
    { "Anniversary", "_anniversary_" },
    { "PTR",         "_ptr_" },
    { "Beta",        "_beta_" },
    { "Classic Beta", "_classic_beta_" },
    { "Forever",     "_forever_" },
};

typedef struct Addon {
    String8 folder;            // folder name inside AddOns
    String8 path;              // full path to the folder
    String8 toc_name;          // .toc file used for metadata
    String8 title;             // WoW color/texture escapes stripped
    String8 version;
    String8 interface_version;
    String8 author;
    String8 notes;
    String8 curse_project_id;  // ## X-Curse-Project-ID
    String8 wago_id;           // ## X-Wago-ID
    String8 wowi_id;           // ## X-WoWI-ID
    String8List required_deps; // ## Dependencies / ## RequiredDeps / ## Dep*: folder names
    U32 fingerprint;
    U32 fingerprint_files;     // how many files went into the fingerprint
    char wago_hash[33];        // Wago fingerprint: MD5 hex over the same files
} Addon;

typedef struct AddonArray { Addon *v; U64 count; U64 files_hashed; B32 fingerprinted; } AddonArray; // fingerprints: addons_fingerprint()

typedef struct GameVersion { U32 major, minor, patch; B32 valid; } GameVersion;
static GameVersion wow_client_version(String8 addons_dir);

static int wow_flavor_index(char *folder)
{
    for (int i = 0; i < (int)(sizeof(wow_flavors) / sizeof(wow_flavors[0])); i += 1) {
        if (strcmp(wow_flavors[i].folder, folder) == 0) return i;
    }
    return 0;
}

// Finds the WoW root by checking common install locations on every drive.
static String8    wow_find_root(Arena *a);   // WAM_WOW_DIR, saved choice, or first found
static String8List wow_find_roots(Arena *a); // every install found
static String8    wow_normalize_root(Arena *a, String8 path);
static B32        wam_save_root(String8 root);
// Accepts a WoW root, a flavor folder, or an AddOns folder; returns the AddOns folder or empty.
static String8    wow_resolve_addons_dir(Arena *a, String8 input, char *flavor_folder);
static AddonArray addons_scan(Arena *a, String8 addons_dir);
static U32        addon_folder_fingerprint(String8 addon_dir, String8 folder_name, OS_FileInfoArray files, U32 *out_file_count, char wago_hash[33]);

#endif
