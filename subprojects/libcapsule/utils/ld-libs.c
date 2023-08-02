// Copyright © 2017-2020 Collabora Ltd
// SPDX-License-Identifier: LGPL-2.1-or-later

// This file is part of libcapsule.

// libcapsule is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.

// libcapsule is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with libcapsule.  If not, see <http://www.gnu.org/licenses/>.

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <link.h>
#include <errno.h>
#include <stdio.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <execinfo.h>

#include "utils.h"
#include "ld-libs.h"


struct dso_cache_search
{
    int idx;
    const char *name;
    ld_libs *ldlibs;
};

static const struct stat *stat_caller (void);

// Note that this is not re-entrant. In this context we don't care.
static const char *
_rtldstr(int flag)
{
    static char flags[160] = { 0 };
    char *f = &flags[0];

    if( !flag)
        return "LOCAL";

#define RTLDFLAGSTR(x) \
    if( x & flag ) f += snprintf(f, &flags[80] - f, " %s", & #x [5])

    RTLDFLAGSTR(RTLD_LAZY);
    RTLDFLAGSTR(RTLD_NOW);
    RTLDFLAGSTR(RTLD_NOLOAD);
    RTLDFLAGSTR(RTLD_DEEPBIND);
    RTLDFLAGSTR(RTLD_GLOBAL);
    RTLDFLAGSTR(RTLD_NODELETE);

    return ( flags[0] == ' ' ) ? &flags[1] : &flags[0];
}

// as we are pulling in files from a non '/' prefix ('/host' by default)
// we need to compensate for this when resolving symlinks.
// this will keep following the path at entry i in ldlibs until
// it finds something that is not a symlink.
static void resolve_symlink_prefixed (ld_libs *ldlibs, int i)
{
    int count = 0;
    char resolved[PATH_MAX];

    assert( ldlibs->prefix.path[ ldlibs->prefix.len ] == '\0' );
    // prefix is unset or is /, nothing to do here (we can rely on
    // libc's built-in symlink following if there's no prefix):
    if( ldlibs->prefix.len == 0 ||
        (ldlibs->prefix.path[0] == '/' && ldlibs->prefix.path[1] == '\0') )
        return;

    LDLIB_DEBUG( ldlibs, DEBUG_PATH,
                 "resolving (un)prefixed link in %s", ldlibs->needed[i].path );

    // set the resolved path to the current needed path as a starting point:
    safe_strncpy( resolved, ldlibs->needed[i].path, PATH_MAX );

    // now keep poking resolve_link (resolved will be updated each time)
    // until it returns false:
    while( resolve_link(ldlibs->prefix.path, resolved) )
    {
        LDLIB_DEBUG( ldlibs, DEBUG_PATH, "  resolved to: %s", resolved );

        if( ++count > MAXSYMLINKS )
        {
            fprintf( stderr, "%s: MAXSYMLINKS (%d) exceeded resolving %s\n",
                     __PRETTY_FUNCTION__, MAXSYMLINKS,
                     ldlibs->needed[i].path );
            break;
        }
    }

    // if the path changed, copy `resolved' back into needed[].path:
    if( count )
        safe_strncpy( ldlibs->needed[i].path, resolved, PATH_MAX );
}


// set the ldlibs elf class and machine based on the link map entry
// passed to us if possible: if we found values for these, return 1,
// otherwise return 0:
static int
find_elf_constraints(ld_libs *ldlibs, struct link_map *m)
{
    int fd = -1;
    Elf *dso = NULL;
    GElf_Ehdr ehdr = { };

    // absolute path or it's a "fake" link map entry which we can't use
    // as there's no actual file to open and inspect:
    if( !m || !m->l_name || (m->l_name[0] != '/'))
        return 0;

    // if we can't open the DSO pointed to by the link map, bail:
    fd = open( m->l_name, O_RDONLY );

    if( fd < 0 )
        return 0;

    dso = elf_begin( fd, ELF_C_READ_MMAP, NULL );

    if( dso && gelf_getehdr( dso, &ehdr ) )
    {
        ldlibs->elf_class   = gelf_getclass( dso );
        ldlibs->elf_machine = ehdr.e_machine;
        DEBUG( DEBUG_SEARCH|DEBUG_CAPSULE,
               "elf class: %d; elf machine: %d; set from: %s",
               ldlibs->elf_class, ldlibs->elf_machine, m->l_name );
    }

    if( dso != NULL )
        elf_end( dso );

    if( fd >= 0 )
        close( fd );

    return ( ldlibs->elf_class != ELFCLASSNONE );
}

// record the class & machine of the start of the link chain
// so that we can only consider matching libraries later
// this matters on multi-arch systems so we don't pick an
// i386 or x32 DSO to statisfy a DT_NEEDED from an x86-64 one.
// return true if we found a valid DSO, false (can't happen?) otherwise
static int
set_elf_constraints (ld_libs *ldlibs)
{
    void *handle;
    struct link_map *map;
    struct link_map *m;

    static int elf_class = ELFCLASSNONE;
    static Elf64_Half elf_machine = EM_NONE;

    if( elf_class != ELFCLASSNONE )
    {
        ldlibs->elf_class   = elf_class;
        ldlibs->elf_machine = elf_machine;
    }
    else if( (handle = dlmopen( LM_ID_BASE, NULL, RTLD_LAZY|RTLD_NOLOAD )) &&
             (dlinfo( handle, RTLD_DI_LINKMAP, &map ) == 0)   )
    {
        // we're not guaranteed to be at the start of the link map chain:
        while( map->l_prev )
            map = map->l_prev;

        // check link maps until we find one we can fill in
        // our constraints from:
        for( m = map; m; m = m->l_next )
            if( find_elf_constraints( ldlibs, m ) )
                break;
    }
    else
    {
        // this would be frankly beyond bizarre:
        fprintf(stderr, "dlopen/dlinfo on self failed: %s\n", dlerror() );
    }

    return ( ( ldlibs->elf_class   != ELFCLASSNONE ) &&
             ( ldlibs->elf_machine |= EM_NONE      ) );
}

// check that the currently opened DSO at offset idx in the needed array
// matches the class & architecture of the DSO we started with:
// return true on a match, false otherwise
static int
check_elf_constraints (ld_libs *ldlibs, int idx, int *code, char **message)
{
    GElf_Ehdr ehdr = {};
    int eclass;

    // bogus ELF DSO - no ehdr available?
    if( !gelf_getehdr( ldlibs->needed[ idx ].dso, &ehdr ) )
    {
        // FIXME: elf_errno() isn't actually in the same domain as errno
        _capsule_set_error( code, message, elf_errno(), "gelf_getehdr(%s): %s",
                            ldlibs->needed[ idx ].path,
                            elf_errmsg( elf_errno() ) );
        return 0;
    }

    eclass = gelf_getclass( ldlibs->needed[ idx ].dso );

    // check class (32 vs 64 bit)
    if( ldlibs->elf_class != eclass )
    {
        _capsule_set_error( code, message, ENOEXEC,
                            "gelf_getclass(%s): expected %d, found %d",
                            ldlibs->needed[ idx ].path, ldlibs->elf_class,
                            eclass );
        return 0;
    }

    // check target architecture (i386, x86-64)
    // x32 ABI is class 32 but machine x86-64
    if( ldlibs->elf_machine != ehdr.e_machine )
    {
        _capsule_set_error( code, message, ENOEXEC,
                            "ehdr.e_machine of %s: expected %d, found %d",
                            ldlibs->needed[ idx ].path, ldlibs->elf_machine,
                            ehdr.e_machine );
        return 0;
    }

    DEBUG( DEBUG_ELF, "constraints: class %d; machine: %d;",
           ldlibs->elf_class, ldlibs->elf_machine );
    DEBUG( DEBUG_ELF, "results    : class %d; machine: %d;",
           eclass, ehdr.e_machine );

    // both the class (word size) and machine (architecture) match
    return 1;
}

// check that the library we're opening is NOT ourself (this is to stop
// a proxy libfoo.so.X from opening itself instead of the real target):
// In normal situations we won't find ourself by accident as the path
// prefix should place the real library ahead of us in the search order.
// However LD_PRELOAD or a weirdly configured system or a missing target
// library could result in the capsule library finding itself and
// inifinitely looping, so we try to avoid that with this test:
static int
target_is_ourself  (ld_libs *ldlibs, int idx)
{
    const struct stat *cdso = stat_caller();
    struct stat xdso = { 0 };

    // Can't check, no stat info for orginal caller.
    // Assume things will mostly be ok, mostly:
    if( cdso == NULL )
        return 0;

    // if stat says we have the same device and inode as
    // the target we're considering ourselves as a target:
    if( fstat( ldlibs->needed[idx].fd, &xdso ) == 0 )
        return ( cdso->st_dev == xdso.st_dev &&
                 cdso->st_ino == xdso.st_ino );

    // placeholder while this test is developed:
    return 0;
}

static void clear_needed (dso_needed_t *needed)
{
    elf_end( needed->dso );
    needed->dso = NULL;

    if( needed->fd >= 0 )
        close( needed->fd );
    needed->fd = -1;

    free( needed->name );
    needed->name = NULL;

    needed->depcount = 0;

    memset( needed->path, 0, PATH_MAX );
    memset( needed->requestors, 0, sizeof(int) * DSO_LIMIT );
}

// open the dso at offset i in the needed array, but only accept it
// if it matches the class & architecture of the starting DSO:
// return a true value only if we finish with a valid fd for the DSO
//
// will set up the needed entry at offset i correctly if we are
// successful, and clear it if we are not:
//
// designed to be called on a populated ldlib needed entry, 'name'
// is the original requested name (typically an absolute path to
// a DSO or a standard DT_NEEDED style specifier following 'libfoo.so.X')
//
// note that this expects the prefix to have already been prepended
// to the DSO path in the ldlibs->needed[i].path buffer if necessary
// this is to allow both prefixed and unprefixed DSOs to be handled
// here:
static int
ld_lib_open (ld_libs *ldlibs, const char *name, int i, int *code, char **message)
{

    LDLIB_DEBUG( ldlibs, DEBUG_SEARCH,
                 "ldlib_open: target -: %s", ldlibs->needed[i].path );

    // resolve the symlink manually if there's a prefix:
    resolve_symlink_prefixed( ldlibs, i );

    LDLIB_DEBUG( ldlibs, DEBUG_SEARCH,
                 "ldlib_open: target +: %s", ldlibs->needed[i].path );

    ldlibs->needed[i].fd = open( ldlibs->needed[i].path, O_RDONLY );

    if( ldlibs->needed[i].fd >= 0 )
    {
        int acceptable = 0;

        ldlibs->needed[i].name = NULL;
        ldlibs->needed[i].dso  =
          elf_begin( ldlibs->needed[i].fd, ELF_C_READ_MMAP, NULL );

        acceptable = check_elf_constraints( ldlibs, i, code, message );

        LDLIB_DEBUG( ldlibs, DEBUG_SEARCH,
                     "[%03d] %s on fd #%d; elf: %p; acceptable: %d",
                     i,
                     ldlibs->needed[i].path,
                     ldlibs->needed[i].fd  ,
                     ldlibs->needed[i].dso ,
                     acceptable );

        if( !acceptable )
        {
            clear_needed( &ldlibs->needed[i] );
            return 0;
        }

        acceptable = !target_is_ourself( ldlibs, i );

        // either clean up the current entry so we can find a better DSO
        // or (for a valid candidate) copy the original requested name in:
        if( !acceptable )
        {
            _capsule_set_error( code, message, EINVAL,
                                "\"%s\" appears to be the libcapsule shim",
                                ldlibs->needed[i].path );
            clear_needed( &ldlibs->needed[i] );
            return 0;
        }

        ldlibs->needed[i].name = xstrdup( name );
        return 1;
    }
    else
    {
        int errsv = errno;

        _capsule_set_error( code, message, errsv,
                            "Cannot open \"%s\": %s",
                            ldlibs->needed[i].path, strerror( errsv ) );
        clear_needed( &ldlibs->needed[i] );
        return 0;
    }
}

// search callback for search_ldcache. see search_ldcache and ld_cache_foreach:
// returning a true value means we found (and set up) the DSO we wanted:
static intptr_t
search_ldcache_cb (const char *name, // name of the DSO in the ldcache
                   int flag,         // 1 for an ELF DSO
                   unsigned int osv, // OS version. we don't use this
                   uint64_t hwcap,   // HW caps. Ibid.
                   const char *path, // absolute path to DSO (may be a symlink)
                   void *data)
{
    struct dso_cache_search *target = data;

    // passed an empty query, just abort the whole search
    if( !target->name || !(*target->name) )
        return 1;

    // what would this even mean? malformed cache entry?
    // skip it and move on
    if( !name || !*name )
        return 0;

    if( strcmp( name, target->name ) == 0 )
    {
        const char *prefix = target->ldlibs->prefix.path;
        int    idx    = target->idx;
        char  *lpath  = target->ldlibs->needed[ idx ].path;

        LDLIB_DEBUG( target->ldlibs, DEBUG_SEARCH|DEBUG_LDCACHE,
                     "checking %s vs %s [%s]",
                     target->name, name, path );
        // copy in the prefix and append the DSO path to it
        if( build_filename( lpath, PATH_MAX, prefix, path, NULL ) >= PATH_MAX )
        {
            return 0;
        }

        // try to open the DSO. This will finish setting up the
        // needed[idx] slot if successful, and reset it ready for
        // another attempt if it fails.
        // TODO: Can we propagate error information?
        if( !ld_lib_open( target->ldlibs, name, idx, NULL, NULL ) )
        {
            // search_ldcache() relies on fd >= 0 iff we succeeded
            assert( target->ldlibs->needed[idx].fd < 0 );
            assert( target->ldlibs->needed[idx].name == NULL );
            return 0;
        }

        // search_ldcache() relies on fd >= 0 iff we succeeded
        assert( target->ldlibs->needed[idx].fd >= 0 );
        assert( target->ldlibs->needed[idx].name != NULL );
        return 1;
    }

    return 0;
}

// search the ld.so.cache loaded into ldlibs for one matching `name'
// name should be unadorned: eg just libfoo.so.X - no path elements
// attached (as the cache lookup is for unadorned library names):
//
// if a match is found, the needed array entry at i will be populated
// and will contain a valid fd for the DSO. (and search_ldcache will
// return true). Otherwise the entry will be empty and we will return false:
//
// this function will respect any path prefix specified in ldlibs
static int
search_ldcache (const char *name, ld_libs *ldlibs, int i)
{
    struct dso_cache_search target;

    target.idx    = i;
    target.name   = name;
    target.ldlibs = ldlibs;

    if( !ldlibs->ldcache.is_open )
    {
        if( !ld_libs_load_cache( ldlibs, NULL, NULL ) )
        {
            // TODO: report error?
            return 0;
        }
    }

    ld_cache_foreach( &ldlibs->ldcache,
                      search_ldcache_cb, &target );

    return ldlibs->needed[i].fd >= 0;
}

// search a : separated path (such as LD_LIBRARY_PATH from the environment)
// for a DSO matching the bare `name' (eg libfoo.so.X)
//
// if a match is found, the needed array entry at i will be populated
// and will contain a valid fd for the DSO. (and search_ldcache will
// return true). Otherwise the entry will be empty and we will return false:
//
// this function will respect any path prefix specified in ldlibs
static int
search_ldpath (const char *name, const char *ldpath, ld_libs *ldlibs, int i)
{
    const char *sp = ldpath;
    const char *prefix = ldlibs->prefix.path;
    size_t plen   = ldlibs->prefix.len;
    char search[PATH_MAX];
    char prefixed[PATH_MAX] = { '\0' };

    assert( prefix[plen] == '\0' );

    LDLIB_DEBUG( ldlibs, DEBUG_SEARCH,
                 "searching for %s in %s (prefix: %s)",
                 name, ldpath, plen ? prefix : "-none-" );

    while( sp && *sp )
    {
        size_t len;
        const char *end;

        end = strchr( sp, ':' );
        if( end )
            len = MIN((size_t) (end - sp), sizeof(search) - 1);
        else
            len = MIN(strlen( sp ), sizeof(search) - 1);

        // If this gets truncated, then there will be no space left in
        // prefixed either, so we'll skip it anyway
        safe_strncpy( search, sp, len + 1 );

        LDLIB_DEBUG( ldlibs, DEBUG_SEARCH, "  searchpath element: %s", search );

        if( build_filename( prefixed, sizeof(prefixed), prefix, search,
                            name, NULL ) >= sizeof(prefixed) )
        {
            LDLIB_DEBUG( ldlibs, DEBUG_SEARCH, "    (too long)" );
        }
        else
        {
            LDLIB_DEBUG( ldlibs, DEBUG_SEARCH, "examining %s", prefixed );
            // if path resolution succeeds _and_ we can open an acceptable
            // DSO at that location, we're good to go (ldlib_open will
            // finish setting up or clearing the needed[] entry for us):
            // FIXME: this is broken if the target is an absolute symlink
            // TODO: Propagate error information from ld_lib_open()?
            if( realpath( prefixed, ldlibs->needed[i].path ) &&
                ld_lib_open( ldlibs, name, i, NULL, NULL ) )
                return 1;
        }

        // search the next path element if there is one
        if( !end )
            break;

        sp = end + 1;
    }

    return 0;
}

// find a DSO using an algorithm that matches the one used by the
// normal dynamic linker and set up the needed array entry at offset i.
//
// Exceptions:
// we don't support DT_RPATH/DT_RUNPATH
// we don't handle ${ORIGIN} and similar
// we will respect any path prefix specified in ldlibs
//
// if a match is found, the needed array entry at i will be populated
// and will contain a valid fd for the DSO. (and search_ldcache will
// return true). Otherwise the entry will be empty and we will return false:
static int
dso_find (const char *name, ld_libs *ldlibs, int i, const char *runpaths, int *code, char **message)
{
    int found = 0;
    const char *ldpath = NULL;
    const char *non_ldlp_paths;
    int absolute = (name && (name[0] == '/'));

    // 'name' is an absolute path, or relative to CWD:
    // we may to need to do some path manipulation
    if( strchr( name, '/' ) )
    {
        char prefixed[PATH_MAX];
        const char *target;

        // we have a path prefix, so yes, we need to do some path manipulation:
        if( ldlibs->prefix.len )
        {
            assert( ldlibs->prefix.path[ ldlibs->prefix.len ] == '\0' );

            if( build_filename( prefixed, sizeof(prefixed),
                                ldlibs->prefix.path, name,
                                NULL ) >= sizeof(prefixed) )
            {
                _capsule_set_error( code, message, ENAMETOOLONG,
                                    "path to DSO is too long: \"%s...\"",
                                    prefixed );
                return 0;
            }
            else if( absolute )
            {
                LDLIB_DEBUG( ldlibs, DEBUG_SEARCH|DEBUG_PATH,
                             "absolute path to DSO %s", prefixed );
            }
            else
            {   // name is relative... this is probably wrong?
                // I don't think this can ever really happen but
                // worst case is we'll simply not open a DSO whose
                // path we couldn't resolve, and then move on
                // (if name is something like ./mylib.so then we'll
                // have treated it as though it was $prefix/./mylib.so)
                LDLIB_DEBUG( ldlibs, DEBUG_SEARCH|DEBUG_PATH,
                             "relative path to DSO %s", prefixed );
            }

            target = prefixed;
        }
        else
        {   // no path prefix, we can look up 'name' directly
            target = name;
        }

        LDLIB_DEBUG( ldlibs, DEBUG_SEARCH|DEBUG_PATH,
                     "resolving path %s", target );
        // this will fail for a non-absolute path, but that's OK
        // if realpath lookup succeeds needed[i].path will be set correctly:
        if( realpath( target, ldlibs->needed[i].path ) )
        {
            return ld_lib_open( ldlibs, name, i, code, message );
        }
        else if( absolute )
        {
            int saved_errno = errno;

            // path was absolute and we couldn't resolve it. give up:
            _capsule_set_error( code, message, saved_errno,
                                "realpath(\"%s\"): %s",
                                ldlibs->needed[i].path,
                                strerror( saved_errno ) );
            return 0;
        }
    }
    else
    {
        // name is a standard bare 'libfoo.so.X' spec:
        assert( !absolute );
    }

    LDLIB_DEBUG( ldlibs, DEBUG_SEARCH, "target DSO is %s", name );

    // Search in the same order that ld.so/libdl use:
    //
    // - DT_RPATH, if no DT_RUNPATH
    // - LD_LIBRARY_PATH
    // - DT_RUNPATH
    // - ld.so.cache
    // - hard-coded directories

    // TODO: Implement DT_RPATH support?
    // If runpaths is null, try the RPATH of the object that
    // resulted in 'name' being loaded, then the RPATH of the object
    // that resulted in that object being loaded, and so on up the hierarchy.
    // We probably don't need this, because modern OSs use DT_RUNPATH.

    if( (ldpath = getenv( "LD_LIBRARY_PATH" )) )
        if( (found = search_ldpath( name, ldpath, ldlibs, i )) )
            return found;

    if( runpaths != NULL )
        if( (found = search_ldpath( name, runpaths, ldlibs, i )) )
            return found;

    if( (found = search_ldcache( name, ldlibs, i )) )
        return found;

    if( (found = search_ldpath( name, "/lib:/usr/lib", ldlibs, i )) )
        return found;

    non_ldlp_paths = "ld.so.cache, DT_RUNPATH or fallback /lib:/usr/lib";

    if (ldpath)
        _capsule_set_error( code, message, ENOENT,
                            "Could not find \"%s\" in LD_LIBRARY_PATH \"%s\", %s",
                            name, ldpath, non_ldlp_paths );
    else
        _capsule_set_error( code, message, ENOENT,
                            "Could not find \"%s\" in LD_LIBRARY_PATH (unset), %s",
                            name, non_ldlp_paths );

    return 0;
}

// if a DSO has already been requested and found as a result of a DT_NEEDED
// entry we've seen before then it's already in the needed array - check
// for such pre-required entries and simply record the dependency instead
// of reopening the DSO (and return true to indicate that we already have the
// DSO)
//
// we have assumed that the root of the DSO chain can never be already-needed
// as this would indicate a circular dependency.
static int
already_needed (dso_needed_t *needed, int requesting_idx, const char *name)
{
    for( int i = DSO_LIMIT - 1; i > 0; i-- )
    {
        if( needed[i].name && strcmp( needed[i].name, name ) == 0)
        {
            DEBUG( DEBUG_CAPSULE, "needed[%d] (%s) also needs needed[%d] (%s)",
                   requesting_idx, needed[requesting_idx].name, i, name );
            needed[i].requestors[requesting_idx] = 1;
            return i;
        }
    }

    return 0;
}

// we're getting to the meat of it: process a DSO at offset idx in the
// needed array, extract each SHT_DYNAMIC section, then make sure we
// can find a DSO to satisfy every DT_NEEDED sub-entry in the section.
// this function recurses into itself each time it finds a previously
// unseen DT_NEEDED value (but not if the DT_NEEDED value is for a DSO
// it has already found and recorded in the needed array)
//
// NOTE: you must use dso_find to seed the 0th entry in the needed array
// or the elf handle in needed[0].dso will not be set up and hilarity*
// will ensue.
static int
_dso_iterate_sections (ld_libs *ldlibs, int idx, int *code, char **error)
{
    int had_error = 0;
    Elf_Scn *scn = NULL;
    dso_needed_t *needed = ldlibs->needed;

    //debug(" ldlibs: %p; idx: %d (%s)", ldlibs, idx, ldlibs->needed[idx].name);

    ldlibs->last_idx = idx;

    LDLIB_DEBUG( ldlibs, DEBUG_ELF,
                 "%03d: fd:%d dso:%p ← %s",
                 idx,
                 needed[idx].fd,
                 needed[idx].dso,
                 needed[idx].path );

    while((scn = elf_nextscn( needed[idx].dso, scn )) != NULL)
    {
        GElf_Shdr shdr = {};
        gelf_getshdr( scn, &shdr );

        // SHT_DYNAMIC is the only section type we care about here:
        if( shdr.sh_type == SHT_DYNAMIC )
        {
            int i = 0;
            GElf_Dyn dyn = {};
            Elf_Data *edata = NULL;
            const char *runpaths = NULL;
            const char *rpaths = NULL;

            edata = elf_getdata( scn, edata );

            // process each DT_* entry in the SHT_DYNAMIC section,
            // looking for DT_RUNPATH or DT_RPATH
            while( !had_error                      &&
                   gelf_getdyn( edata, i++, &dyn ) &&
                   (dyn.d_tag != DT_NULL)          )
            {

                if( dyn.d_tag == DT_RUNPATH )
                {
                    runpaths = elf_strptr( needed[idx].dso, shdr.sh_link,
                                           dyn.d_un.d_val );
                    LDLIB_DEBUG( ldlibs, DEBUG_SEARCH|DEBUG_ELF,
                                 "%s DT_RUNPATHs \"%s\"",
                                 needed[idx].name, runpaths );
                }
                else if( dyn.d_tag == DT_RPATH )
                {
                    rpaths = elf_strptr( needed[idx].dso, shdr.sh_link,
                                         dyn.d_un.d_val );
                    LDLIB_DEBUG( ldlibs, DEBUG_SEARCH|DEBUG_ELF,
                                 "TODO: Not using %s DT_RPATHs \"%s\"",
                                 needed[idx].name, rpaths );
                }
            }

            i = 0;

            // process each DT_* entry in the SHT_DYNAMIC section again,
            // this time looking for DT_NEEDED
            while( !had_error                      &&
                   gelf_getdyn( edata, i++, &dyn ) &&
                   (dyn.d_tag != DT_NULL)          )
            {
                int skip = 0;
                int next = ldlibs->last_idx;
                char *next_dso; // name of the dependency we're going to need
                char *local_error = NULL;

                // we're only gathering DT_NEEDED (dependency) entries here:
                if( dyn.d_tag != DT_NEEDED )
                    continue;

                next_dso =
                  elf_strptr( needed[idx].dso, shdr.sh_link, dyn.d_un.d_val );

                //////////////////////////////////////////////////
                // ignore the linker itself
                if( strstr( next_dso, "ld-" ) == next_dso )
                    continue;

                // ignore any DSOs we've been specifically told to leave out:
                for( char **x = (char **)ldlibs->exclude; x && *x; x++ )
                {
                    if( strcmp( *x, next_dso ) == 0 )
                    {
                        LDLIB_DEBUG( ldlibs, DEBUG_SEARCH|DEBUG_ELF,
                                     "skipping %s / %s", next_dso, *x );
                        skip = 1;
                        break;
                    }
                }

                if( skip )
                    continue;

                //////////////////////////////////////////////////
                // if we got this far, we have another dependency:
                needed[idx].depcount++;

                // already on our list, no need to do anything else here:
                if( already_needed( needed, idx, next_dso ) )
                    continue;

                next++;
                if( next >= DSO_LIMIT )
                {
                    had_error = 1;
                    _capsule_set_error_literal( code, error, ELIBMAX,
                                                "Too many dependencies" );
                    break;
                }

                if( !dso_find( next_dso, ldlibs, next, runpaths, code, &local_error ) )
                {
                    had_error = 1;
                    ldlibs->not_found[ ldlibs->last_not_found++ ] =
                      _capsule_steal_pointer( &local_error );
                    // Avoid "piling up" errors which would be a memory
                    // leak
                    if( error != NULL && *error == NULL )
                        *error = xstrdup( "Missing dependencies:" );
                }
                else
                {
                    LDLIB_DEBUG( ldlibs, DEBUG_CAPSULE,
                                 "needed[%d] (%s) is the first to need needed[%d] (%s)",
                                 idx, needed[idx].name, next, next_dso );
                    // record which DSO requested the new library we found:
                    needed[next].requestors[idx] = 1;
                    // now find the dependencies of our newest dependency:
                    _dso_iterate_sections( ldlibs, next, code, error );
                }
            }
        }
    }

    return !had_error;
}

static void
_dso_iterator_format_error (ld_libs * ldlibs, char **message)
{
    size_t extra_space = 0;

    assert( message );
    assert( *message );

    if( ! ldlibs->not_found[0] )
        return;

    for( int i = 0; (i < DSO_LIMIT) && ldlibs->not_found[i]; i++ )
        extra_space += strlen( ldlibs->not_found[i] ) + 1;

    if( extra_space )
    {
        char *append_here;
        char *end;
        size_t prev_space = strlen( *message );

        *message = xrealloc( *message, prev_space + extra_space + 2 );
        append_here = *message + prev_space;
        end = *message + prev_space + extra_space + 1;
        memset( append_here, 0, extra_space + 2 );

        for( int i = 0; (i < DSO_LIMIT) && ldlibs->not_found[i]; i++ )
        {
            append_here +=
              snprintf( append_here, end - append_here,
                        " %s", ldlibs->not_found[i] );
            free( ldlibs->not_found[i] );
            ldlibs->not_found[i] = NULL;
        }
    }
}

// wrapper to format any accumulated errors and similar after
// invoking the actual dso iterator: returns true if we gathered
// all the needed info witout error, false otherwise:
static int
dso_iterate_sections (ld_libs *ldlibs, int idx, int *code, char **message)
{
    if( _dso_iterate_sections( ldlibs, idx, code, message ) )
        return 1;

    if( message )
        _dso_iterator_format_error( ldlibs, message );

    return 0;
}


////////////////////////////////////////////////////////////////////////////
// public API:
int
ld_libs_init (ld_libs *ldlibs,
              const char **exclude,
              const char *prefix,
              unsigned long dbg,
              int *errcode,
              char **error)
{
    memset( ldlibs, 0, sizeof(ld_libs) );

    ld_cache_close( &ldlibs->ldcache );

    ldlibs->elf_class   = ELFCLASSNONE;
    ldlibs->elf_machine = EM_NONE;
    ldlibs->exclude     = exclude;
    ldlibs->debug       = dbg;

    if( errcode )
        *errcode = 0;

    if( error )
        *error = NULL;

    // super important, 0 is valid but is usually stdin,
    // don't want to go stomping all over that by accident:
    for( int x = 0; x < DSO_LIMIT; x++ )
        ldlibs->needed[x].fd = -1;

    if( elf_version(EV_CURRENT) == EV_NONE )
    {
        // FIXME: elf_errno() isn't actually in the same domain as errno
        _capsule_set_error( errcode, error, elf_errno(),
                            "elf_version(EV_CURRENT): %s",
                            elf_errmsg( elf_errno() ) );
        return 0;
    }

    stat_caller();
    set_elf_constraints( ldlibs );
    // ==================================================================
    // set up the path prefix at which we expect to find the encapsulated
    // library and its ld.so.cache and dependencies and so forth:
    if( prefix && prefix[0] != '\0' )
    {
        size_t prefix_len = strlen( prefix );
        ssize_t space = PATH_MAX - prefix_len;

        if( prefix[0] != '/' )
        {
            _capsule_set_error( errcode, error, EINVAL,
                                "An absolute path is required, not \"%s\"",
                                prefix );
        }

        // if we don't have at least this much space it's not
        // going to work out:
        if( (space - strlen( "/usr/lib/libx.so.x" )) <= 0 )
        {
            _capsule_set_error_literal( errcode, error, ENAMETOOLONG,
                                        "capsule_dlmopen: prefix is too large" );
            return 0;
        }

        // This now can't get truncated
        safe_strncpy( ldlibs->prefix.path, prefix, PATH_MAX );
        ldlibs->prefix.len = prefix_len;
    }
    else
    {
        ldlibs->prefix.path[0] = '\0';
        ldlibs->prefix.len     = 0;
    }

    return 1;
}

// set up the starting point for our capsule load
// (ie the DSO we actually want):
// returns false on error:
int
ld_libs_set_target (ld_libs *ldlibs, const char *target, int *code, char **message)
{
    return dso_find( target, ldlibs, 0, NULL, code, message );
}

int
ld_libs_find_dependencies (ld_libs *ldlibs, int *code, char **message)
{
    return dso_iterate_sections( ldlibs, 0, code, message );
}

// And now we actually open everything we have found, in reverse
// dependency order (which prevents dlmopen from going and finding
// DT_NEEDED values from outside the capsule), which it will do
// if we don't work backwards:
void *
ld_libs_load (ld_libs *ldlibs, Lmid_t *namespace, int flag, int *error,
              char **message)
{
    int go;
    Lmid_t lm = (*namespace >= 0) ? *namespace : LM_ID_NEWLM;
    void *ret = NULL;

    if( !flag )
        flag = RTLD_LAZY;

    do
    {
        go = 0;

        for( int j = 0; j < DSO_LIMIT; j++ )
        {
            // reached the end of the list
            if( !ldlibs->needed[j].name )
                continue;

            // library has no further dependencies which have not already
            // been satisfied (except for the libc and linker DSOs),
            // this means we can safely open it without dlmopen accidentally
            // pulling in DSOs from outside the encapsulated tree:
            if( ldlibs->needed[j].depcount == 0 )
            {
                const char *path = ldlibs->needed[j].path;
                go++;

                LDLIB_DEBUG( ldlibs, DEBUG_CAPSULE,
                             "DLMOPEN needed[%d]: %p %s %s",
                             j, (void *)lm, _rtldstr(flag), path );

                // The actual dlmopen. If this was the first one, it may
                // have created a new link map id, wich we record later on:

                // note that since we do the opens in reverse dependency order,
                // the _last_ one we open will be the DSO we actually asked for
                // so if we succeed, ret has to contain the right handle.
                ret = dlmopen( lm, path, flag );

                if( !ret )
                {
                    if (lm == LM_ID_NEWLM)
                        _capsule_set_error( error, message, EINVAL,
                                            "dlmopen(LM_ID_NEWLM, \"%s\", %s): %s",
                                            path, _rtldstr( flag ), dlerror() );
                    else
                        _capsule_set_error( error, message, EINVAL,
                                            "dlmopen(%p, \"%s\", %s): %s",
                                            (void *) lm, path,
                                            _rtldstr( flag ), dlerror() );

                    return NULL;
                }

                // If this was the first dlmopen, record the new LM Id
                // for return to our caller:
                if( lm == LM_ID_NEWLM )
                {
                    dlinfo( ret, RTLD_DI_LMID, namespace );
                    lm = *namespace;
                    LDLIB_DEBUG( ldlibs, DEBUG_CAPSULE,
                                 "new Lmid_t handle %p\n", (void *)lm );
                }

                // go through the map of DSOs and reduce the dependency
                // count for any DSOs which had the current DSO as a dep:
                for( int k = 0; k < DSO_LIMIT; k++ )
                {
                    if( ldlibs->needed[j].requestors[k] )
                    {
                        ldlibs->needed[k].depcount--;
                        LDLIB_DEBUG( ldlibs, DEBUG_CAPSULE,
                                     "needed[%d] (%s) dependency on \"%s\" "
                                     "satisfied, %d more libraries needed",
                                     k, ldlibs->needed[k].name, path,
                                     ldlibs->needed[k].depcount );
                    }
                }

                LDLIB_DEBUG( ldlibs, DEBUG_CAPSULE,
                             "Forgetting about needed[%d] (%s)",
                             j, ldlibs->needed[j].name );
                clear_needed( &ldlibs->needed[j] );
            }
        }
    } while (go);

    if( !ret )
    {
        _capsule_set_error( error, message, EINVAL,
                            "How do we get here? go = 0" );
        return NULL;
    }

    return ret;
}

void
ld_libs_finish (ld_libs *ldlibs)
{
    for( int i = 0; i < DSO_LIMIT; i++ )
        clear_needed( &ldlibs->needed[i] );

    for( int i = ldlibs->last_not_found; i >= 0; i-- )
    {
        free( ldlibs->not_found[i] );
        ldlibs->not_found[i] = NULL;
    }

    ldlibs->last_not_found = 0;

    ld_cache_close( &ldlibs->ldcache );

    ldlibs->last_idx = 0;
    ldlibs->elf_class = ELFCLASSNONE;
    ldlibs->elf_machine = EM_NONE;

    ldlibs->prefix.len = 0;
    ldlibs->prefix.path[0] = '\0';
}

// this walks backwards along (up? down?) the stack until it finds
// a DSO with a base address that differs from its own, then caches
// the stat info for said path.
// This is to support the libcapsule-proxies-will-not-reopen-themselves
// infinite loop prevention mechanism.
static const struct stat *
stat_caller (void)
{
    static struct stat cdso = { 0 };
    static int done = 0;

    Dl_info self = { 0 };
    char caller[PATH_MAX] = { '\0' };

    if( !done && dladdr( ld_libs_init, &self ) )
    {
        void *trace[16] = { NULL };
        Dl_info dso     = { 0 };
        void *origin    = self.dli_fbase;
        int traced;

        traced = backtrace( trace, N_ELEMENTS( trace ) );

        for( int x = 0; x < traced && caller[0] == '\0'; x++ )
            if( dladdr( trace[x], &dso ) )
                if( origin != dso.dli_fbase )
                    safe_strncpy( caller, dso.dli_fname, PATH_MAX );

        done = 1;

        if( caller[0] == '\0' )
        {
            DEBUG( DEBUG_SEARCH|DEBUG_ELF,
                   "unable to test for infinitely-looped capsule, "
                   "proceeding anyway" );
        }
        else
        {
            DEBUG( DEBUG_SEARCH|DEBUG_ELF,
                   "initial libcapsule user is %s", caller );
            if( soname_matches_path( "libc.so", caller ) )
            {
                // if we get back to here we're looking at a normal
                // library from one of the build helper tools and
                // we don't care about loop protection:
                DEBUG( DEBUG_SEARCH|DEBUG_ELF,
                       "  … which is not a capsule: disabling loop protection" );
            }
            else
            {
                stat( caller, &cdso );
            }
        }
    }

    return (cdso.st_size > 0) ? &cdso : NULL;
}

// map the ld.so.cache for the system into memory so that we can search it
// for DSOs in the same way as the dynamic linker.
//
// returns true on success, false otherwise.
//
// this function respects any path prefix specified in ldlibs
int
ld_libs_load_cache (ld_libs *libs, int *code, char **message)
{
    char prefixed[PATH_MAX] = { '\0' };
    size_t i;

    for( i = 0; ld_cache_filenames[i] != NULL; i++ )
    {
        if( message != NULL )
            _capsule_clear( message );

        if( build_filename( prefixed, sizeof(prefixed), libs->prefix.path,
                            ld_cache_filenames[i], NULL ) >= sizeof(prefixed) )
        {
            _capsule_set_error( code, message, ENAMETOOLONG,
                                "Cannot append \"%s\" to prefix \"%s\": too long",
                                ld_cache_filenames[i], libs->prefix.path );
            continue;
        }

        if( ld_cache_open( &libs->ldcache, prefixed, code, message ) )
            return 1;
    }

    // return the last error
    return 0;
}
