#include "capsule/capsule.h"
#include "capsule/capsule-private.h"
#include "utils/utils.h"
#include "utils/ld-libs.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>

static int
dso_is_exported (const char *dsopath, char **exported)
{
    for( char **ex = exported; ex && *ex; ex++ )
        if( soname_matches_path( *ex, dsopath ) )
            return 1;

    return 0;
}

static void *
_dlsym_from_capsules (const char *symbol)
{
    void *addr = NULL;

    for( size_t n = 0; n < _capsule_list->next; n++ )
    {
        capsule cap = ptr_list_nth_ptr( _capsule_list, n );

        if( !cap )
            continue;

        // TODO: If handle != cap->dl_handle, should we skip it?
        // TODO: RTLD_NEXT isn't implemented (is it implementable?)
        addr = _capsule_original_dlsym ( cap->dl_handle, symbol );

        if( addr )
        {
            Dl_info dso = { 0 };

            // only keep addr from the capsule if it's from an exported DSO:
            // or if we are unable to determine where it came from (what?)
            if( dladdr( addr, &dso ) )
            {
                if( !dso_is_exported( dso.dli_fname, cap->ns->combined_export ) )
                    addr = NULL;

                DEBUG( DEBUG_DLFUNC|DEBUG_WRAPPERS,
                       "symbol %s is from soname %s - %s",
                       symbol, dso.dli_fname, addr ? "OK" : "Ignored" );

                if( addr )
                    break;
            }
        }
    }

    return addr;
}

static int
_dlsymbol_is_encapsulated (const void *addr)
{
    Dl_info dso = { 0 };

    // no info, symbol may not even be valid:
    if( !dladdr( addr, &dso ) )
        return 0;

    // no file name, can't be a shim:
    if( !dso.dli_fname || *dso.dli_fname == '\0' )
        return 0;

    // check to see if addr came from a registered capsule:
    for( size_t n = 0; n < _capsule_list->next; n++ )
    {
        capsule cap = ptr_list_nth_ptr( _capsule_list, n );

        if( cap && soname_matches_path( cap->meta->soname, dso.dli_fname) )
            return 1;
    }

    return 0;
}

// TODO: Implement dlvsym()?
// TODO: RTLD_NEXT needs special handling

// revised algorithm here:
//
// use the vanilla dlsym
// if nothing is found, peek into the whole capsule; return the result
//
// if a symbol is found, check to see if it came from a shim
// if it did (ie it is a dummy), peek into the capsule as above
// if it did not, return what was found
//
// The main weakness here is that if the caller expects to find a
// symbol XYZ via ‘handle’ which does _not_ come from the capsule
// but the capsule also has a symbol XYZ which is from an explicitly
// exported-from soname then the caller will get the capsule's
// XYZ symbol.
//
// We can't just check for RTLD_DEFAULT as the handle since
// dlopen( NULL, … ) and/or the RTLD_GLOBAL flag can be used to
// promote symbols that would otherwise not be visible from a given
// handle (libGL does this).
void *
capsule_external_dlsym (void *handle, const char *symbol)
{
    DEBUG( DEBUG_DLFUNC|DEBUG_WRAPPERS, "dlsym(%s)", symbol );
    void *addr = _capsule_original_dlsym ( handle, symbol );

    // nothing found, must be from a capsule or nowhere at all:
    if( !addr )
    {
        DEBUG( DEBUG_DLFUNC|DEBUG_WRAPPERS,
               "%s not found, searching capsule", symbol );
        addr = _dlsym_from_capsules( symbol );
        DEBUG( DEBUG_DLFUNC|DEBUG_WRAPPERS,
               "capsule %s has address %p", symbol, addr );
        return addr;
    }

    // found something. is it a dummy symbol from a shim?
    if( _dlsymbol_is_encapsulated( addr ) )
    {
        DEBUG( DEBUG_DLFUNC|DEBUG_WRAPPERS,
               "dummy %s found, searching capsule", symbol );
        addr = _dlsym_from_capsules( symbol );
        DEBUG( DEBUG_DLFUNC|DEBUG_WRAPPERS,
               "capsule %s has address %p", symbol, addr );
        return addr;
    }

    DEBUG( DEBUG_DLFUNC|DEBUG_WRAPPERS,
           "vanilla %s found at %p", symbol, addr );
    return addr;
}

void *
capsule_external_dlopen(const char *file, int flag)
{
    void *handle = NULL;
    char *error  = NULL;

    if( _capsule_original_dlopen )
    {
        handle = _capsule_original_dlopen ( file, flag );
    }
    else
    {
        fprintf( stderr,
                 "capsule_external_dlopen() has no dlopen() implementation\n" );
        abort();
    }

    if( handle != NULL )
    {
        unsigned long df = debug_flags;

        if( debug_flags & DEBUG_DLFUNC )
            debug_flags |= DEBUG_RELOCS;
        // This may not even be necessary, so it should not be fatal.
        // We do want to log it though as it might be an important clue:
        for( size_t n = 0; n < _capsule_list->next; n++ )
        {
            const capsule c = ptr_list_nth_ptr( _capsule_list, n );

            if( !c )
                continue;

            if( _capsule_relocate( c, &error ) != 0 )
            {
                fprintf( stderr,
                         "relocation from %s after dlopen(%s, …) failed: %s\n",
                         c->meta->soname, file, error );
                free( error );
            }

            if( _capsule_relocate_dlopen( c, &error ) != 0 )
            {
                fprintf( stderr,
                         "dl-wrapper relocation from %s after "
                         "dlopen(%s, …) failed: %s\n",
                         c->meta->soname, file, error );
                free( error );
            }
        }

        debug_flags = df;
    }

    return handle;
}

void *
capsule_shim_dlopen(const capsule cap, const char *file, int flag)
{
    void *res = NULL;
    int code = 0;
    char *errors = NULL;
    ld_libs ldlibs = {};

    DEBUG( DEBUG_WRAPPERS|DEBUG_DLFUNC,
           "dlopen(%s, %x) wrapper: LMID: %ld; prefix: %s;",
           file, flag, cap->ns->ns, cap->ns->prefix );

    if( cap->ns->prefix && strcmp(cap->ns->prefix, "/") )
    {
        if( !ld_libs_init( &ldlibs, (const char **)cap->ns->combined_exclude,
                           cap->ns->prefix, debug_flags, &code, &errors ) )
        {
            DEBUG( DEBUG_LDCACHE|DEBUG_WRAPPERS|DEBUG_DLFUNC,
                   "Initialising ld_libs data failed: error %d: %s",
                   code, errors);
              goto cleanup;
        }

        if( !ld_libs_load_cache( &ldlibs, "/etc/ld.so.cache", &code, &errors ) )
        {
            DEBUG( DEBUG_LDCACHE|DEBUG_WRAPPERS|DEBUG_DLFUNC,
                   "Loading ld.so.cache from %s: error %d: %s", cap->ns->prefix,
                   code, errors );
            goto cleanup;
        }

        // find the initial DSO (ie what the caller actually asked for):
        if( !ld_libs_set_target( &ldlibs, file, &code, &errors ) )
        {
            DEBUG( DEBUG_SEARCH|DEBUG_WRAPPERS|DEBUG_DLFUNC,
                           "Not found: %s under %s: error %d: %s",
                           file, cap->ns->prefix, code, errors );
            goto cleanup;
        }

        // harvest all the requested DSO's dependencies:
        if( !ld_libs_find_dependencies( &ldlibs, &code, &errors ) )
        {
            DEBUG( DEBUG_WRAPPERS|DEBUG_DLFUNC,
                   "capsule dlopen error %d: %s", code, errors );
            goto cleanup;
        }

        // load them up in reverse dependency order:
        res = ld_libs_load( &ldlibs, &cap->ns->ns, flag, &code, &errors );

        if( !res )
            DEBUG( DEBUG_WRAPPERS|DEBUG_DLFUNC,
                   "capsule dlopen error %d: %s", code, errors );

        goto cleanup;
    }
    else // no prefix: straightforward dlmopen into our capsule namespace:
    {
        res = dlmopen( cap->ns->ns, file, flag );

        if( !res )
            DEBUG( DEBUG_WRAPPERS|DEBUG_DLFUNC,
                   "capsule dlopen error %s: %s", file, dlerror() );
    }

    return res;

cleanup:
    ld_libs_finish( &ldlibs );
    free( errors );
    return res;
}

////////////////////////////////////////////////////////////////////////////
// copy some vodoo out of libc.
// it is to be hoped that this is a temporary hack but, well…
#define SIZE_SZ (sizeof(size_t))

struct malloc_chunk
{
  size_t               prev_size;  /* Size of previous chunk (if free).  */
  size_t               size;       /* Size in bytes, including overhead. */

  struct malloc_chunk* fd;         /* double links -- used only if free. */
  struct malloc_chunk* bk;

  struct malloc_chunk* fd_nextsize; /* double links -- used only if free. */
  struct malloc_chunk* bk_nextsize;
};

typedef struct malloc_chunk* mchunkptr;

#define chunk2mem(p)   ((void*)((char*)(p) + 2*SIZE_SZ))
#define mem2chunk(mem) ((mchunkptr)((char*)(mem) - 2*SIZE_SZ))

/* size field is or'ed with IS_MMAPPED if the chunk was obtained with mmap() */
#define IS_MMAPPED 0x2
/* check for mmap()'ed chunk */
#define chunk_is_mmapped(p) ((p)->size & IS_MMAPPED)
////////////////////////////////////////////////////////////////////////////

void
capsule_shim_free (const capsule cap, void *ptr)
{
    static ElfW(Addr) base = (ElfW(Addr)) NULL;
    ElfW(Addr) top;

    if( !ptr )
        return;

    top = (ElfW(Addr)) sbrk( 0 );

    if( base == (ElfW(Addr)) NULL )
    {
        struct mallinfo mi = mallinfo();
        base = top - (ElfW(Addr)) mi.arena;
    }

    // it's from the main heap, comes from the vanilla libc outside
    // the capsule
    if( (base < (ElfW(Addr)) ptr) && ((ElfW(Addr)) ptr < top) )
    {
        free( ptr );
        return;
    }

    mchunkptr p = mem2chunk( ptr );

    // mmapped pointer/chunk: can't tell whose this is but since we
    // override the malloc/free cluster as early as possible we're
    // kind of hoping we don't have any of these from inside the capsule
    //
    // we'd only have such a pointer if the libraries we dlmopen() into
    // the capsule allocated large chunks of memory in their initialiser(s):
    if (chunk_is_mmapped(p))
    {
        free( ptr );
        return;
    }

    // probably from the pseudo heap inside the capsule?
    cap->ns->free( ptr );
}
