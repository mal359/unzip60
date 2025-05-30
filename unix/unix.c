/*
  Copyright (c) 1990-2009 Info-ZIP.  All rights reserved.

  See the accompanying file LICENSE, version 2009-Jan-02 or later
  (the contents of which are also included in unzip.h) for terms of use.
  If, for some reason, all these files are missing, the Info-ZIP license
  also may be found at:  ftp://ftp.info-zip.org/pub/infozip/license.html
*/
/*---------------------------------------------------------------------------

  unix.c

  Unix-specific routines for use with Info-ZIP's UnZip 5.41 and later.

  Contains:  readdir()
             do_wild()           <-- generic enough to put in fileio.c?
             mapattr()
             mapname()
             checkdir()
             mkdir()
             close_outfile()
             defer_dir_attribs()
             set_direc_attribs()
             stamp_file()
             version()

  ---------------------------------------------------------------------------*/


#define UNZIP_INTERNAL
#include "unzip.h"

#include <iconv.h>
#include <langinfo.h>
#include <stdbool.h>

#ifdef SCO_XENIX
#  define SYSNDIR
#else  /* SCO Unix, AIX, DNIX, TI SysV, Coherent 4.x, ... */
#  if defined(__convexc__) || defined(SYSV) || defined(CRAY) || defined(BSD4_4)
#    define DIRENT
#  endif
#endif
#if defined(_AIX) || defined(__mpexl)
#  define DIRENT
#endif
#ifdef COHERENT
#  if defined(_I386) || (defined(__COHERENT__) && (__COHERENT__ >= 0x420))
#    define DIRENT
#  endif
#endif

#ifdef __HAIKU__
#include <fs_attr.h>
#include <ByteOrder.h>
#include <Mime.h>
#define EB_BE_FL_BADBITS    0xfe    /* bits currently undefined */
static uch *scanBeOSexfield  OF((const uch *ef_ptr, unsigned ef_len));
static void setBeOSexfield   OF((const char *path, uch *extra_field));
#ifdef BEOS_USE_PRINTEXFIELD
static void printBeOSexfield OF((int isdir, uch *extra_field));
#endif
#ifdef BEOS_ASSIGN_FILETYPE
static void assign_MIME( const char * );
#endif
#endif

#ifdef _POSIX_VERSION
#  ifndef DIRENT
#    define DIRENT
#  endif
#endif

#ifdef DIRENT
#  include <dirent.h>
#else
#  ifdef SYSV
#    ifdef SYSNDIR
#      include <sys/ndir.h>
#    else
#      include <ndir.h>
#    endif
#  else /* !SYSV */
#    ifndef NO_SYSDIR
#      include <sys/dir.h>
#    endif
#  endif /* ?SYSV */
#  ifndef dirent
#    define dirent direct
#  endif
#endif /* ?DIRENT */

#ifdef SET_DIR_ATTRIB
typedef struct uxdirattr {      /* struct for holding unix style directory */
    struct uxdirattr *next;     /*  info until can be sorted and set at end */
    char *fn;                   /* filename of directory */
    union {
        iztimes t3;             /* mtime, atime, ctime */
        ztimbuf t2;             /* modtime, actime */
    } u;
    unsigned perms;             /* same as min_info.file_attr */
    int have_uidgid;            /* flag */
    ulg uidgid[2];
    char fnbuf[1];              /* buffer stub for directory name */
} uxdirattr;
#define UxAtt(d)  ((uxdirattr *)d)    /* typecast shortcut */
#endif /* SET_DIR_ATTRIB */

#ifdef ACORN_FTYPE_NFS
/* Acorn bits for NFS filetyping */
typedef struct {
  uch ID[2];
  uch size[2];
  uch ID_2[4];
  uch loadaddr[4];
  uch execaddr[4];
  uch attr[4];
} RO_extra_block;

#endif /* ACORN_FTYPE_NFS */

/* static int created_dir;      */      /* used in mapname(), checkdir() */
/* static int renamed_fullpath; */      /* ditto */

static unsigned filtattr OF((__GPRO__ unsigned perms));


/*****************************/
/* Strings used multiple     */
/* times in unix.c           */
/*****************************/

#ifndef MTS
/* messages of code for setting file/directory attributes */
static ZCONST char CannotSetItemUidGid[] =
  "warning:  cannot set UID %lu and/or GID %lu for %s\n          %s\n";
static ZCONST char CannotSetUidGid[] =
  " (warning) cannot set UID %lu and/or GID %lu\n          %s";
static ZCONST char CannotSetItemTimestamps[] =
  "warning:  cannot set modif./access times for %s\n          %s\n";
static ZCONST char CannotSetTimestamps[] =
  " (warning) cannot set modif./access times\n          %s";
#endif /* !MTS */


#ifndef SFX
#ifdef NO_DIR                  /* for AT&T 3B1 */

#define opendir(path) fopen(path,"r")
#define closedir(dir) fclose(dir)
typedef FILE DIR;
typedef struct zdir {
    FILE *dirhandle;
    struct dirent *entry;
} DIR
DIR *opendir OF((ZCONST char *dirspec));
void closedir OF((DIR *dirp));
struct dirent *readdir OF((DIR *dirp));

DIR *opendir(dirspec)
    ZCONST char *dirspec;
{
    DIR *dirp;

    if ((dirp = malloc(sizeof(DIR)) != NULL) {
        if ((dirp->dirhandle = fopen(dirspec, "r")) == NULL) {
            free(dirp);
            dirp = NULL;
        }
    }
    return dirp;
}

void closedir(dirp)
    DIR *dirp;
{
    fclose(dirp->dirhandle);
    free(dirp);
}

/*
 *  Apparently originally by Rich Salz.
 *  Cleaned up and modified by James W. Birdsall.
 */
struct dirent *readdir(dirp)
    DIR *dirp;
{

    if (dirp == NULL)
        return NULL;

    for (;;)
        if (fread(&(dirp->entry), sizeof (struct dirent), 1,
                  dirp->dirhandle) == 0)
            return (struct dirent *)NULL;
        else if ((dirp->entry).d_ino)
            return &(dirp->entry);

} /* end function readdir() */

#endif /* NO_DIR */


/**********************/
/* Function do_wild() */   /* for porting: dir separator; match(ignore_case) */
/**********************/

char *do_wild(__G__ wildspec)
    __GDEF
    ZCONST char *wildspec;  /* only used first time on a given dir */
{
/* these statics are now declared in SYSTEM_SPECIFIC_GLOBALS in unxcfg.h:
    static DIR *wild_dir = (DIR *)NULL;
    static ZCONST char *wildname;
    static char *dirname, matchname[FILNAMSIZ];
    static int notfirstcall=FALSE, have_dirname, dirnamelen;
*/
    struct dirent *file;

    /* Even when we're just returning wildspec, we *always* do so in
     * matchname[]--calling routine is allowed to append four characters
     * to the returned string, and wildspec may be a pointer to argv[].
     */
    if (!G.notfirstcall) {  /* first call:  must initialize everything */
        G.notfirstcall = TRUE;

        if (!iswild(wildspec)) {
            strncpy(G.matchname, wildspec, FILNAMSIZ);
            G.matchname[FILNAMSIZ-1] = '\0';
            G.have_dirname = FALSE;
            G.wild_dir = NULL;
            return G.matchname;
        }

        /* break the wildspec into a directory part and a wildcard filename */
        if ((G.wildname = (ZCONST char *)strrchr(wildspec, '/')) == NULL) {
            G.dirname = ".";
            G.dirnamelen = 1;
            G.have_dirname = FALSE;
            G.wildname = wildspec;
        } else {
            ++G.wildname;     /* point at character after '/' */
            G.dirnamelen = G.wildname - wildspec;
            if ((G.dirname = (char *)malloc(G.dirnamelen+1)) == (char *)NULL) {
                Info(slide, 0x201, ((char *)slide,
                  "warning:  cannot allocate wildcard buffers\n"));
                strncpy(G.matchname, wildspec, FILNAMSIZ);
                G.matchname[FILNAMSIZ-1] = '\0';
                return G.matchname; /* but maybe filespec was not a wildcard */
            }
            strncpy(G.dirname, wildspec, G.dirnamelen);
            G.dirname[G.dirnamelen] = '\0';   /* terminate for strcpy below */
            G.have_dirname = TRUE;
        }

        if ((G.wild_dir = (zvoid *)opendir(G.dirname)) != (zvoid *)NULL) {
            while ((file = readdir((DIR *)G.wild_dir)) !=
                   (struct dirent *)NULL) {
                Trace((stderr, "do_wild:  readdir returns %s\n",
                  FnFilter1(file->d_name)));
                if (file->d_name[0] == '.' && G.wildname[0] != '.')
                    continue; /* Unix:  '*' and '?' do not match leading dot */
                if (match(file->d_name, G.wildname, 0 WISEP) &&/*0=case sens.*/
                    /* skip "." and ".." directory entries */
                    strcmp(file->d_name, ".") && strcmp(file->d_name, "..")) {
                    Trace((stderr, "do_wild:  match() succeeds\n"));
                    if (G.have_dirname) {
                        strcpy(G.matchname, G.dirname);
                        strcpy(G.matchname+G.dirnamelen, file->d_name);
                    } else
                        strcpy(G.matchname, file->d_name);
                    return G.matchname;
                }
            }
            /* if we get to here directory is exhausted, so close it */
            closedir((DIR *)G.wild_dir);
            G.wild_dir = (zvoid *)NULL;
        }
        Trace((stderr, "do_wild:  opendir(%s) returns NULL\n",
          FnFilter1(G.dirname)));

        /* return the raw wildspec in case that works (e.g., directory not
         * searchable, but filespec was not wild and file is readable) */
        strncpy(G.matchname, wildspec, FILNAMSIZ);
        G.matchname[FILNAMSIZ-1] = '\0';
        return G.matchname;
    }

    /* last time through, might have failed opendir but returned raw wildspec */
    if ((DIR *)G.wild_dir == (DIR *)NULL) {
        G.notfirstcall = FALSE; /* nothing left--reset for new wildspec */
        if (G.have_dirname)
            free(G.dirname);
        return (char *)NULL;
    }

    /* If we've gotten this far, we've read and matched at least one entry
     * successfully (in a previous call), so dirname has been copied into
     * matchname already.
     */
    while ((file = readdir((DIR *)G.wild_dir)) != (struct dirent *)NULL) {
        Trace((stderr, "do_wild:  readdir returns %s\n",
          FnFilter1(file->d_name)));
        if (file->d_name[0] == '.' && G.wildname[0] != '.')
            continue;   /* Unix:  '*' and '?' do not match leading dot */
        if (match(file->d_name, G.wildname, 0 WISEP)) { /* 0 == case sens. */
            Trace((stderr, "do_wild:  match() succeeds\n"));
            if (G.have_dirname) {
                /* strcpy(G.matchname, G.dirname); */
                strcpy(G.matchname+G.dirnamelen, file->d_name);
            } else
                strcpy(G.matchname, file->d_name);
            return G.matchname;
        }
    }

    closedir((DIR *)G.wild_dir);  /* at least one entry read; nothing left */
    G.wild_dir = (zvoid *)NULL;
    G.notfirstcall = FALSE;       /* reset for new wildspec */
    if (G.have_dirname)
        free(G.dirname);
    return (char *)NULL;

} /* end function do_wild() */

#endif /* !SFX */




#ifndef S_ISUID
# define S_ISUID        0004000 /* set user id on execution */
#endif
#ifndef S_ISGID
# define S_ISGID        0002000 /* set group id on execution */
#endif
#ifndef S_ISVTX
# define S_ISVTX        0001000 /* save swapped text even after use */
#endif
#ifndef UNX_IWGRP
# define UNX_IWGRP      00020   /* Unix write permission: group */
#endif
#ifndef UNX_IWOTH
# define UNX_IWOTH      00002   /* Unix write permission: other */
#endif

/************************/
/*  Function filtattr() */
/************************/
/* This is used to clear or keep the SUID and SGID bits on file permissions.
 * It's possible that a file in an archive could have one of these bits set
 * and, unknown to the person unzipping, could allow others to execute the
 * file as the user or group.  The new option -K bypasses this check.
 */

static unsigned filtattr(__G__ perms)
    __GDEF
    unsigned perms;
{
    /* keep setuid/setgid/tacky perms? */
    if (!uO.K_flag)
        perms &= ~(S_ISUID | S_ISGID | S_ISVTX);

    /* Clear IWGRP and IWOTH bits on file permissions. It's possible that a file
     * in an archive could have one of these bits set and could allow others to
     * overwrite or append to the file. */
    perms &= ~(UNX_IWGRP | UNX_IWOTH);

    return (0xffff & perms);
} /* end function filtattr() */





/**********************/
/* Function mapattr() */
/**********************/

int mapattr(__G)
    __GDEF
{
    int r;
    ulg tmp = G.crec.external_file_attributes;

    G.pInfo->file_attr = 0;
    /* initialized to 0 for check in "default" branch below... */

    switch (G.pInfo->hostnum) {
        case AMIGA_:
            tmp = (unsigned)(tmp>>17 & 7);   /* Amiga RWE bits */
            G.pInfo->file_attr = (unsigned)(tmp<<6 | tmp<<3 | tmp);
            break;
        case THEOS_:
            tmp &= 0xF1FFFFFFL;
            if ((tmp & 0xF0000000L) != 0x40000000L)
                tmp &= 0x01FFFFFFL;     /* not a dir, mask all ftype bits */
            else
                tmp &= 0x41FFFFFFL;     /* leave directory bit as set */
            /* fall through! */
        case UNIX_:
        case VMS_:
        case ACORN_:
        case ATARI_:
        case ATHEOS_:
        case BEOS_:
        case QDOS_:
        case TANDEM_:
            r = FALSE;
            G.pInfo->file_attr = (unsigned)(tmp >> 16);
            if (G.pInfo->file_attr == 0 && G.extra_field) {
                /* Some (non-Info-ZIP) implementations of Zip for Unix and
                 * VMS (and probably others ??) leave 0 in the upper 16-bit
                 * part of the external_file_attributes field. Instead, they
                 * store file permission attributes in some extra field.
                 * As a work-around, we search for the presence of one of
                 * these extra fields and fall back to the MSDOS compatible
                 * part of external_file_attributes if one of the known
                 * e.f. types has been detected.
                 * Later, we might implement extraction of the permission
                 * bits from the VMS extra field. But for now, the work-around
                 * should be sufficient to provide "readable" extracted files.
                 * (For ASI Unix e.f., an experimental remap of the e.f.
                 * mode value IS already provided!)
                 */
                ush ebID;
                unsigned ebLen;
                uch *ef = G.extra_field;
                unsigned ef_len = G.crec.extra_field_length;

                while (!r && ef_len >= EB_HEADSIZE) {
                    ebID = makeword(ef);
                    ebLen = (unsigned)makeword(ef+EB_LEN);
                    if (ebLen > (ef_len - EB_HEADSIZE))
                        /* discoverd some e.f. inconsistency! */
                        break;
                    switch (ebID) {
                      case EF_ASIUNIX:
                        if (ebLen >= (EB_ASI_MODE+2)) {
                            G.pInfo->file_attr =
                              (unsigned)makeword(ef+(EB_HEADSIZE+EB_ASI_MODE));
                            /* force stop of loop: */
                            ef_len = (ebLen + EB_HEADSIZE);
                            break;
                        }
                        /* else: fall through! */
                      case EF_PKVMS:
                        /* "found nondecypherable e.f. with perm. attr" */
                        r = TRUE;
                      default:
                        break;
                    }
                    ef_len -= (ebLen + EB_HEADSIZE);
                    ef += (ebLen + EB_HEADSIZE);
                }
            }
            if (!r) {
#ifdef SYMLINKS
                /* Check if the file is a (POSIX-compatible) symbolic link.
                 * We restrict symlink support to those "made-by" hosts that
                 * are known to support symbolic links.
                 */
                G.pInfo->symlink = S_ISLNK(G.pInfo->file_attr) &&
                                   SYMLINK_HOST(G.pInfo->hostnum);
#endif
                return 0;
            }
            /* fall through! */
        /* all remaining cases:  expand MSDOS read-only bit into write perms */
        case FS_FAT_:
            /* PKWARE's PKZip for Unix marks entries as FS_FAT_, but stores the
             * Unix attributes in the upper 16 bits of the external attributes
             * field, just like Info-ZIP's Zip for Unix.  We try to use that
             * value, after a check for consistency with the MSDOS attribute
             * bits (see below).
             */
            G.pInfo->file_attr = (unsigned)(tmp >> 16);
            /* fall through! */
        case FS_HPFS_:
        case FS_NTFS_:
        case MAC_:
        case TOPS20_:
        default:
            /* Ensure that DOS subdir bit is set when the entry's name ends
             * in a '/'.  Some third-party Zip programs fail to set the subdir
             * bit for directory entries.
             */
            if ((tmp & 0x10) == 0) {
                extent fnlen = strlen(G.filename);
                if (fnlen > 0 && G.filename[fnlen-1] == '/')
                    tmp |= 0x10;
            }
            /* read-only bit --> write perms; subdir bit --> dir exec bit */
            tmp = !(tmp & 1) << 1  |  (tmp & 0x10) >> 4;
            if ((G.pInfo->file_attr & 0700) == (unsigned)(0400 | tmp<<6)) {
                /* keep previous G.pInfo->file_attr setting, when its "owner"
                 * part appears to be consistent with DOS attribute flags!
                 */
#ifdef SYMLINKS
                /* Entries "made by FS_FAT_" could have been zipped on a
                 * system that supports POSIX-style symbolic links.
                 */
                G.pInfo->symlink = S_ISLNK(G.pInfo->file_attr) &&
                                   (G.pInfo->hostnum == FS_FAT_);
#endif
                return 0;
            }
            G.pInfo->file_attr = (unsigned)(0444 | tmp<<6 | tmp<<3 | tmp);
            break;
    } /* end switch (host-OS-created-by) */

    /* for originating systems with no concept of "group," "other," "system": */
    umask( (int)(tmp=umask(0)) );    /* apply mask to expanded r/w(/x) perms */
    G.pInfo->file_attr &= ~tmp;

    return 0;

} /* end function mapattr() */





/************************/
/*  Function mapname()  */
/************************/

int mapname(__G__ renamed)
    __GDEF
    int renamed;
/*
 * returns:
 *  MPN_OK          - no problem detected
 *  MPN_INF_TRUNC   - caution (truncated filename)
 *  MPN_INF_SKIP    - info "skip entry" (dir doesn't exist)
 *  MPN_ERR_SKIP    - error -> skip entry
 *  MPN_ERR_TOOLONG - error -> path is too long
 *  MPN_NOMEM       - error (memory allocation failed) -> skip entry
 *  [also MPN_VOL_LABEL, MPN_CREATED_DIR]
 */
{
    char pathcomp[FILNAMSIZ];      /* path-component buffer */
    char *pp, *cp=(char *)NULL;    /* character pointers */
    char *lastsemi=(char *)NULL;   /* pointer to last semi-colon in pathcomp */
#ifdef ACORN_FTYPE_NFS
    char *lastcomma=(char *)NULL;  /* pointer to last comma in pathcomp */
    RO_extra_block *ef_spark;      /* pointer Acorn FTYPE ef block */
#endif
    int killed_ddot = FALSE;       /* is set when skipping "../" pathcomp */
    int error = MPN_OK;
    register unsigned workch;      /* hold the character being tested */


/*---------------------------------------------------------------------------
    Initialize various pointers and counters and stuff.
  ---------------------------------------------------------------------------*/

    if (G.pInfo->vollabel)
        return MPN_VOL_LABEL;   /* can't set disk volume labels in Unix */

    /* can create path as long as not just freshening, or if user told us */
    G.create_dirs = (!uO.fflag || renamed);

    G.created_dir = FALSE;      /* not yet */

    /* user gave full pathname:  don't prepend rootpath */
    G.renamed_fullpath = (renamed && (*G.filename == '/'));

    if (checkdir(__G__ (char *)NULL, INIT) == MPN_NOMEM)
        return MPN_NOMEM;       /* initialize path buffer, unless no memory */

    *pathcomp = '\0';           /* initialize translation buffer */
    pp = pathcomp;              /* point to translation buffer */
    if (uO.jflag)               /* junking directories */
        cp = (char *)strrchr(G.filename, '/');
    if (cp == (char *)NULL)     /* no '/' or not junking dirs */
        cp = G.filename;        /* point to internal zipfile-member pathname */
    else
        ++cp;                   /* point to start of last component of path */

/*---------------------------------------------------------------------------
    Begin main loop through characters in filename.
  ---------------------------------------------------------------------------*/

    while ((workch = (uch)*cp++) != 0) {

        switch (workch) {
            case '/':             /* can assume -j flag not given */
                *pp = '\0';
                if (strcmp(pathcomp, ".") == 0) {
                    /* don't bother appending "./" to the path */
                    *pathcomp = '\0';
                } else if (!uO.ddotflag && strcmp(pathcomp, "..") == 0) {
                    /* "../" dir traversal detected, skip over it */
                    *pathcomp = '\0';
                    killed_ddot = TRUE;     /* set "show message" flag */
                }
                /* when path component is not empty, append it now */
                if (*pathcomp != '\0' &&
                    ((error = checkdir(__G__ pathcomp, APPEND_DIR))
                     & MPN_MASK) > MPN_INF_TRUNC)
                    return error;
                pp = pathcomp;    /* reset conversion buffer for next piece */
                lastsemi = (char *)NULL; /* leave direct. semi-colons alone */
                break;

#ifdef __CYGWIN__   /* Cygwin runs on Win32, apply FAT/NTFS filename rules */
            case '\\':        /* '\\' may come as normal filename char (not */
                *pp++ = '_';  /* these rules apply equally to FAT and NTFS */
                break;
#endif

            case ';':             /* VMS version (or DEC-20 attrib?) */
                lastsemi = pp;
                *pp++ = ';';      /* keep for now; remove VMS ";##" */
                break;            /*  later, if requested */

#ifdef ACORN_FTYPE_NFS
            case ',':             /* NFS filetype extension */
                lastcomma = pp;
                *pp++ = ',';      /* keep for now; may need to remove */
                break;            /*  later, if requested */
#endif

#ifdef MTS
            case ' ':             /* change spaces to underscore under */
                *pp++ = '_';      /*  MTS; leave as spaces under Unix */
                break;
#endif

            default:
                /* disable control character filter when requested,
                 * else allow 8-bit characters (e.g. UTF-8) in filenames:
                 */
                if (uO.cflxflag ||
                    (isprint(workch) || (128 <= workch && workch <= 255)))
                    *pp++ = (char)workch;
        } /* end switch */

    } /* end while loop */

    /* Show warning when stripping insecure "parent dir" path components */
    if (killed_ddot && QCOND2) {
        Info(slide, 0, ((char *)slide,
          "warning:  skipped \"../\" path component(s) in %s\n",
          FnFilter1(G.filename)));
        if (!(error & ~MPN_MASK))
            error = (error & MPN_MASK) | PK_WARN;
    }

/*---------------------------------------------------------------------------
    Report if directory was created (and no file to create:  filename ended
    in '/'), check name to be sure it exists, and combine path and name be-
    fore exiting.
  ---------------------------------------------------------------------------*/

    if (G.filename[strlen(G.filename) - 1] == '/') {
        checkdir(__G__ G.filename, GETPATH);
        if (G.created_dir) {
            if (QCOND2) {
                Info(slide, 0, ((char *)slide, "   creating: %s\n",
                  FnFilter1(G.filename)));
            }
#ifdef __HAIKU__
            if (!uO.J_flag) {   /* Handle the BeOS extra field if present. */
                void *ptr = scanBeOSexfield(G.extra_field,
                                            G.lrec.extra_field_length);
                if (ptr) {
                    setBeOSexfield(G.filename, ptr);
                }
            }
#endif
#ifndef NO_CHMOD
            /* Filter out security-relevant attributes bits. */
            G.pInfo->file_attr = filtattr(__G__ G.pInfo->file_attr);
            /* When extracting non-UNIX directories or when extracting
             * without UID/GID restoration or SGID preservation, any
             * SGID flag inherited from the parent directory should be
             * maintained to allow files extracted into this new folder
             * to inherit the GID setting from the parent directory.
             */
            if (G.pInfo->hostnum != UNIX_ || !(uO.X_flag || uO.K_flag)) {
                /* preserve SGID bit when inherited from parent dir */
                if (!SSTAT(G.filename, &G.statbuf)) {
                    G.pInfo->file_attr |= G.statbuf.st_mode & S_ISGID;
                } else {
                    perror("Could not read directory attributes");
                }
            }

            /* set approx. dir perms (make sure can still read/write in dir) */
            /* clearing IWGRP and IWOTH bits so that these permissions are not
             * accidentally allowed. */
            if (chmod(G.filename, (G.pInfo->file_attr | 0700) & ~(UNX_IWGRP | 
            	UNX_IWOTH)))
                perror("chmod (directory attributes) error");
#endif
            /* set dir time (note trailing '/') */
            return (error & ~MPN_MASK) | MPN_CREATED_DIR;
        }
#ifdef __HAIKU__
        /* TODO: should we re-write the BeOS extra field data in case it's */
        /* changed?  The answer is yes. [Sept 1999 - cjh]                  */
        if (!uO.J_flag) {   /* Handle the BeOS extra field if present. */
            void *ptr = scanBeOSexfield(G.extra_field,
                                        G.lrec.extra_field_length);
            if (ptr) {
                setBeOSexfield(G.filename, ptr);
            }
        }
#endif
        /* dir existed already; don't look for data to extract */
        return (error & ~MPN_MASK) | MPN_INF_SKIP;
    }

    *pp = '\0';                   /* done with pathcomp:  terminate it */

    /* if not saving them, remove VMS version numbers (appended ";###") */
    if (!uO.V_flag && lastsemi) {
        pp = lastsemi + 1;
        if (*pp != '\0') {        /* At least one digit is required. */       
        while (isdigit((uch)(*pp)))
            ++pp;
        if (*pp == '\0')          /* only digits between ';' and end:  nuke */
            *lastsemi = '\0';
        }
    }

    /* On UNIX (and compatible systems), "." and ".." are reserved for
     * directory navigation and cannot be used as regular file names.
     * These reserved one-dot and two-dot names are mapped to "_" and "__".
     */
    if (strcmp(pathcomp, ".") == 0)
        *pathcomp = '_';
    else if (strcmp(pathcomp, "..") == 0)
        strcpy(pathcomp, "__");

#ifdef ACORN_FTYPE_NFS
    /* translate Acorn filetype information if asked to do so */
    if (uO.acorn_nfs_ext &&
        (ef_spark = (RO_extra_block *)
                    getRISCOSexfield(G.extra_field, G.lrec.extra_field_length))
        != (RO_extra_block *)NULL)
    {
        /* file *must* have a RISC OS extra field */
        long ft = (long)makelong(ef_spark->loadaddr);
        /*32-bit*/
        if (lastcomma) {
            pp = lastcomma + 1;
            while (isxdigit((uch)(*pp))) ++pp;
            if (pp == lastcomma+4 && *pp == '\0') *lastcomma='\0'; /* nuke */
        }
        if ((ft & 1<<31)==0) ft=0x000FFD00;
        sprintf(pathcomp+strlen(pathcomp), ",%03x", (int)(ft>>8) & 0xFFF);
    }
#endif /* ACORN_FTYPE_NFS */

    if (*pathcomp == '\0') {
        Info(slide, 1, ((char *)slide, "mapname:  conversion of %s failed\n",
          FnFilter1(G.filename)));
        return (error & ~MPN_MASK) | MPN_ERR_SKIP;
    }

    checkdir(__G__ pathcomp, APPEND_NAME);  /* returns 1 if truncated: care? */
    checkdir(__G__ G.filename, GETPATH);

    return error;

} /* end function mapname() */




#if 0  /*========== NOTES ==========*/

  extract-to dir:      a:path/
  buildpath:           path1/path2/ ...   (NULL-terminated)
  pathcomp:                filename

  mapname():
    loop over chars in zipfile member name
      checkdir(path component, COMPONENT | CREATEDIR) --> map as required?
        (d:/tmp/unzip/)                    (disk:[tmp.unzip.)
        (d:/tmp/unzip/jj/)                 (disk:[tmp.unzip.jj.)
        (d:/tmp/unzip/jj/temp/)            (disk:[tmp.unzip.jj.temp.)
    finally add filename itself and check for existence? (could use with rename)
        (d:/tmp/unzip/jj/temp/msg.outdir)  (disk:[tmp.unzip.jj.temp]msg.outdir)
    checkdir(name, GETPATH)     -->  copy path to name and free space

#endif /* 0 */




/***********************/
/* Function checkdir() */
/***********************/

int checkdir(__G__ pathcomp, flag)
    __GDEF
    char *pathcomp;
    int flag;
/*
 * returns:
 *  MPN_OK          - no problem detected
 *  MPN_INF_TRUNC   - (on APPEND_NAME) truncated filename
 *  MPN_INF_SKIP    - path doesn't exist, not allowed to create
 *  MPN_ERR_SKIP    - path doesn't exist, tried to create and failed; or path
 *                    exists and is not a directory, but is supposed to be
 *  MPN_ERR_TOOLONG - path is too long
 *  MPN_NOMEM       - can't allocate memory for filename buffers
 */
{
 /* static int rootlen = 0; */  /* length of rootpath */
 /* static char *rootpath;  */  /* user's "extract-to" directory */
 /* static char *buildpath; */  /* full path (so far) to extracted file */
 /* static char *end;       */  /* pointer to end of buildpath ('\0') */

#   define FN_MASK   7
#   define FUNCTION  (flag & FN_MASK)



/*---------------------------------------------------------------------------
    APPEND_DIR:  append the path component to the path being built and check
    for its existence.  If doesn't exist and we are creating directories, do
    so for this one; else signal success or error as appropriate.
  ---------------------------------------------------------------------------*/

    if (FUNCTION == APPEND_DIR) {
        int too_long = FALSE;
#ifdef SHORT_NAMES
        char *old_end = end;
#endif

        Trace((stderr, "appending dir segment [%s]\n", FnFilter1(pathcomp)));
        while ((*G.end = *pathcomp++) != '\0')
            ++G.end;
#ifdef SHORT_NAMES   /* path components restricted to 14 chars, typically */
        if ((G.end-old_end) > FILENAME_MAX)  /* GRR:  proper constant? */
            *(G.end = old_end + FILENAME_MAX) = '\0';
#endif

        /* GRR:  could do better check, see if overrunning buffer as we go:
         * check end-buildpath after each append, set warning variable if
         * within 20 of FILNAMSIZ; then if var set, do careful check when
         * appending.  Clear variable when begin new path. */

        /* next check: need to append '/', at least one-char name, '\0' */
        if ((G.end-G.buildpath) > FILNAMSIZ-3)
            too_long = TRUE;                    /* check if extracting dir? */
        if (SSTAT(G.buildpath, &G.statbuf)) {   /* path doesn't exist */
            if (!G.create_dirs) { /* told not to create (freshening) */
                free(G.buildpath);
                return MPN_INF_SKIP;    /* path doesn't exist: nothing to do */
            }
            if (too_long) {
                Info(slide, 1, ((char *)slide,
                  "checkdir error:  path too long: %s\n",
                  FnFilter1(G.buildpath)));
                free(G.buildpath);
                /* no room for filenames:  fatal */
                return MPN_ERR_TOOLONG;
            }
            if (mkdir(G.buildpath, 0777) == -1) {   /* create the directory */
                Info(slide, 1, ((char *)slide,
                  "checkdir error:  cannot create %s\n\
                 %s\n\
                 unable to process %s.\n",
                  FnFilter2(G.buildpath),
                  strerror(errno),
                  FnFilter1(G.filename)));
                free(G.buildpath);
                /* path didn't exist, tried to create, failed */
                return MPN_ERR_SKIP;
            }
            G.created_dir = TRUE;
        } else if (!S_ISDIR(G.statbuf.st_mode)) {
            Info(slide, 1, ((char *)slide,
              "checkdir error:  %s exists but is not directory\n\
                 unable to process %s.\n",
              FnFilter2(G.buildpath), FnFilter1(G.filename)));
            free(G.buildpath);
            /* path existed but wasn't dir */
            return MPN_ERR_SKIP;
        }
        if (too_long) {
            Info(slide, 1, ((char *)slide,
              "checkdir error:  path too long: %s\n", FnFilter1(G.buildpath)));
            free(G.buildpath);
            /* no room for filenames:  fatal */
            return MPN_ERR_TOOLONG;
        }
        *G.end++ = '/';
        *G.end = '\0';
        Trace((stderr, "buildpath now = [%s]\n", FnFilter1(G.buildpath)));
        return MPN_OK;

    } /* end if (FUNCTION == APPEND_DIR) */

/*---------------------------------------------------------------------------
    GETPATH:  copy full path to the string pointed at by pathcomp, and free
    G.buildpath.
  ---------------------------------------------------------------------------*/

    if (FUNCTION == GETPATH) {
        strcpy(pathcomp, G.buildpath);
        Trace((stderr, "getting and freeing path [%s]\n",
          FnFilter1(pathcomp)));
        free(G.buildpath);
        G.buildpath = G.end = (char *)NULL;
        return MPN_OK;
    }

/*---------------------------------------------------------------------------
    APPEND_NAME:  assume the path component is the filename; append it and
    return without checking for existence.
  ---------------------------------------------------------------------------*/

    if (FUNCTION == APPEND_NAME) {
#ifdef SHORT_NAMES
        char *old_end = end;
#endif

        Trace((stderr, "appending filename [%s]\n", FnFilter1(pathcomp)));
        while ((*G.end = *pathcomp++) != '\0') {
            ++G.end;
#ifdef SHORT_NAMES  /* truncate name at 14 characters, typically */
            if ((G.end-old_end) > FILENAME_MAX)    /* GRR:  proper constant? */
                *(G.end = old_end + FILENAME_MAX) = '\0';
#endif
            if ((G.end-G.buildpath) >= FILNAMSIZ) {
                *--G.end = '\0';
                Info(slide, 0x201, ((char *)slide,
                  "checkdir warning:  path too long; truncating\n\
                   %s\n                -> %s\n",
                  FnFilter1(G.filename), FnFilter2(G.buildpath)));
                return MPN_INF_TRUNC;   /* filename truncated */
            }
        }
        Trace((stderr, "buildpath now = [%s]\n", FnFilter1(G.buildpath)));
        /* could check for existence here, prompt for new name... */
        return MPN_OK;
    }

/*---------------------------------------------------------------------------
    INIT:  allocate and initialize buffer space for the file currently being
    extracted.  If file was renamed with an absolute path, don't prepend the
    extract-to path.
  ---------------------------------------------------------------------------*/

/* GRR:  for VMS and TOPS-20, add up to 13 to strlen */

    if (FUNCTION == INIT) {
        Trace((stderr, "initializing buildpath to "));
#ifdef ACORN_FTYPE_NFS
        if ((G.buildpath = (char *)malloc(strlen(G.filename)+G.rootlen+
                                          (uO.acorn_nfs_ext ? 5 : 1)))
#else
        if ((G.buildpath = (char *)malloc(strlen(G.filename)+G.rootlen+1))
#endif
            == (char *)NULL)
            return MPN_NOMEM;
        if ((G.rootlen > 0) && !G.renamed_fullpath) {
            strcpy(G.buildpath, G.rootpath);
            G.end = G.buildpath + G.rootlen;
        } else {
            *G.buildpath = '\0';
            G.end = G.buildpath;
        }
        Trace((stderr, "[%s]\n", FnFilter1(G.buildpath)));
        return MPN_OK;
    }

/*---------------------------------------------------------------------------
    ROOT:  if appropriate, store the path in rootpath and create it if
    necessary; else assume it's a zipfile member and return.  This path
    segment gets used in extracting all members from every zipfile specified
    on the command line.
  ---------------------------------------------------------------------------*/

#if (!defined(SFX) || defined(SFX_EXDIR))
    if (FUNCTION == ROOT) {
        Trace((stderr, "initializing root path to [%s]\n",
          FnFilter1(pathcomp)));
        if (pathcomp == (char *)NULL) {
            G.rootlen = 0;
            return MPN_OK;
        }
        if (G.rootlen > 0)      /* rootpath was already set, nothing to do */
            return MPN_OK;
        if ((G.rootlen = strlen(pathcomp)) > 0) {
            char *tmproot;

            if ((tmproot = (char *)malloc(G.rootlen+2)) == (char *)NULL) {
                G.rootlen = 0;
                return MPN_NOMEM;
            }
            strcpy(tmproot, pathcomp);
            if (tmproot[G.rootlen-1] == '/') {
                tmproot[--G.rootlen] = '\0';
            }
            if (G.rootlen > 0 && (SSTAT(tmproot, &G.statbuf) ||
                                  !S_ISDIR(G.statbuf.st_mode)))
            {   /* path does not exist */
                if (!G.create_dirs /* || iswild(tmproot) */ ) {
                    free(tmproot);
                    G.rootlen = 0;
                    /* skip (or treat as stored file) */
                    return MPN_INF_SKIP;
                }
                /* create the directory (could add loop here scanning tmproot
                 * to create more than one level, but why really necessary?) */
                if (mkdir(tmproot, 0777) == -1) {
                    Info(slide, 1, ((char *)slide,
                      "checkdir:  cannot create extraction directory: %s\n\
           %s\n",
                      FnFilter1(tmproot), strerror(errno)));
                    free(tmproot);
                    G.rootlen = 0;
                    /* path didn't exist, tried to create, and failed: */
                    /* file exists, or 2+ subdir levels required */
                    return MPN_ERR_SKIP;
                }
            }
            tmproot[G.rootlen++] = '/';
            tmproot[G.rootlen] = '\0';
            if ((G.rootpath = (char *)realloc(tmproot, G.rootlen+1)) == NULL) {
                free(tmproot);
                G.rootlen = 0;
                return MPN_NOMEM;
            }
            Trace((stderr, "rootpath now = [%s]\n", FnFilter1(G.rootpath)));
        }
        return MPN_OK;
    }
#endif /* !SFX || SFX_EXDIR */

/*---------------------------------------------------------------------------
    END:  free rootpath, immediately prior to program exit.
  ---------------------------------------------------------------------------*/

    if (FUNCTION == END) {
        Trace((stderr, "freeing rootpath\n"));
        if (G.rootlen > 0) {
            free(G.rootpath);
            G.rootlen = 0;
        }
        return MPN_OK;
    }

    return MPN_INVALID; /* should never reach */

} /* end function checkdir() */





#ifdef NO_MKDIR

/********************/
/* Function mkdir() */
/********************/

int mkdir(path, mode)
    ZCONST char *path;
    int mode;   /* ignored */
/*
 * returns:   0 - successful
 *           -1 - failed (errno not set, however)
 */
{
    char command[FILNAMSIZ+40]; /* buffer for system() call */

    /* GRR 930416:  added single quotes around path to avoid bug with
     * creating directories with ampersands in name; not yet tested */
    sprintf(command, "IFS=\" \t\n\" /bin/mkdir '%s' 2>/dev/null", path);
    if (system(command))
        return -1;
    return 0;
}

#endif /* NO_MKDIR */




#if (!defined(MTS) || defined(SET_DIR_ATTRIB))
static int get_extattribs OF((__GPRO__ iztimes *pzt, ulg z_uidgid[2]));

static int get_extattribs(__G__ pzt, z_uidgid)
    __GDEF
    iztimes *pzt;
    ulg z_uidgid[2];
{
/*---------------------------------------------------------------------------
    Convert from MSDOS-format local time and date to Unix-format 32-bit GMT
    time:  adjust base year from 1980 to 1970, do usual conversions from
    yy/mm/dd hh:mm:ss to elapsed seconds, and account for timezone and day-
    light savings time differences.  If we have a Unix extra field, however,
    we're laughing:  both mtime and atime are ours.  On the other hand, we
    then have to check for restoration of UID/GID.
  ---------------------------------------------------------------------------*/
    int have_uidgid_flg;
    unsigned eb_izux_flg;

    eb_izux_flg = (G.extra_field ? ef_scan_for_izux(G.extra_field,
                   G.lrec.extra_field_length, 0, G.lrec.last_mod_dos_datetime,
#ifdef IZ_CHECK_TZ
                   (G.tz_is_valid ? pzt : NULL),
#else
                   pzt,
#endif
                   z_uidgid) : 0);
    if (eb_izux_flg & EB_UT_FL_MTIME) {
        TTrace((stderr, "\nget_extattribs:  Unix e.f. modif. time = %ld\n",
          pzt->mtime));
    } else {
        pzt->mtime = dos_to_unix_time(G.lrec.last_mod_dos_datetime);
    }
    if (eb_izux_flg & EB_UT_FL_ATIME) {
        TTrace((stderr, "get_extattribs:  Unix e.f. access time = %ld\n",
          pzt->atime));
    } else {
        pzt->atime = pzt->mtime;
        TTrace((stderr, "\nget_extattribs:  modification/access times = %ld\n",
          pzt->mtime));
    }

    /* if -X option was specified and we have UID/GID info, restore it */
    have_uidgid_flg =
#ifdef RESTORE_UIDGID
            (uO.X_flag && (eb_izux_flg & EB_UX2_VALID));
#else
            0;
#endif
    return have_uidgid_flg;
}
#endif /* !MTS || SET_DIR_ATTRIB */



#ifndef MTS

/****************************/
/* Function CloseError()    */
/***************************/

int CloseError(__G)
    __GDEF
{
    int errval = PK_OK;
    
    if (fclose(G.outfile) < 0) {
          switch (errno) {
                case ENOSPC:
                    /* Do we need this on fileio.c? */
                    Info(slide, 0x4a1, ((char *)slide, "%s: write error (disk full?).   Continue? (y/n/^C) ",
                          FnFilter1(G.filename)));
                    fgets(G.answerbuf, 9, stdin);
                    if (*G.answerbuf == 'y')     /* stop writing to this file */
                        G.disk_full = 1;         /* pass to next */
                    else
                        G.disk_full = 2;         /* no: exit program */
          
                    errval = PK_DISK;
                    break;

                default:
                    errval = PK_WARN;
          }
     }
     return errval;
} /* End of CloseError() */

/****************************/
/* Function close_outfile() */
/****************************/

int close_outfile(__G) 
    __GDEF
{
    union {
        iztimes t3;             /* mtime, atime, ctime */
        ztimbuf t2;             /* modtime, actime */
    } zt;
    ulg z_uidgid[2];
    int have_uidgid_flg;
    int errval = PK_OK;

    have_uidgid_flg = get_extattribs(__G__ &(zt.t3), z_uidgid);

/*---------------------------------------------------------------------------
    If symbolic links are supported, allocate storage for a symlink control
    structure, put the uncompressed "data" and other required info in it, and
    add the structure to the "deferred symlinks" chain.  Since we know it's a
    symbolic link to start with, we shouldn't have to worry about overflowing
    unsigned ints with unsigned longs.
  ---------------------------------------------------------------------------*/

#ifdef SYMLINKS
    if (G.symlnk) {
        extent ucsize = (extent)G.lrec.ucsize;
# ifdef SET_SYMLINK_ATTRIBS
        extent attribsize = sizeof(unsigned) +
                            (have_uidgid_flg ? sizeof(z_uidgid) : 0);
# else
        extent attribsize = 0;
# endif
        extent slnk_entrysize;
        slinkentry *slnk_entry;
#ifdef __HAIKU__
		uch *BeOS_exfld;
        if (!uO.J_flag) {
            /* Symlinks can have attributes, too. */
            BeOS_exfld = scanBeOSexfield(G.extra_field,
                                         G.lrec.extra_field_length);
            if (BeOS_exfld) {
                attribsize = makeword(EB_LEN + BeOS_exfld) + EB_HEADSIZE;
            }
        }
#endif
        /* size of the symlink entry is the sum of
         *  (struct size (includes 1st '\0') + 1 additional trailing '\0'),
         *  system specific attribute data size (might be 0),
         *  and the lengths of name and link target.
         */
        slnk_entrysize = (sizeof(slinkentry) + 1) + attribsize +
                                ucsize + strlen(G.filename);
        
        if (slnk_entrysize < ucsize) {
            Info(slide, 0x201, ((char *)slide,
              "warning:  symbolic link (%s) failed: mem alloc overflow\n",
              FnFilter1(G.filename)));
            errval = CloseError(G.outfile, G.filename);
            return errval ? errval : PK_WARN;
        }

        if ((slnk_entry = (slinkentry *)malloc(slnk_entrysize)) == NULL) {
            Info(slide, 0x201, ((char *)slide,
              "warning:  symbolic link (%s) failed: no mem\n",
              FnFilter1(G.filename)));
            errval = CloseError(G.outfile, G.filename);
            return errval ? errval : PK_WARN;
        }
        slnk_entry->next = NULL;
        slnk_entry->targetlen = ucsize;
        slnk_entry->attriblen = attribsize;
# ifdef SET_SYMLINK_ATTRIBS
#ifdef __HAIKU__
        if (attribsize > sizeof(unsigned))
            memcpy(slnk_entry->buf, BeOS_exfld, attribsize);
#else        
            memcpy(slnk_entry->buf, &(G.pInfo->file_attr),
               sizeof(unsigned));
        if (have_uidgid_flg)
            memcpy(slnk_entry->buf + 4, z_uidgid, sizeof(z_uidgid));
#endif
# endif
        slnk_entry->target = slnk_entry->buf + slnk_entry->attriblen;
        slnk_entry->fname = slnk_entry->target + ucsize + 1;
        strcpy(slnk_entry->fname, G.filename);
        /* move back to the start of the file to re-read the "link data" */
        rewind(G.outfile);

        if (fread(slnk_entry->target, 1, ucsize, G.outfile) != ucsize)
        {
            Info(slide, 0x201, ((char *)slide,
              "warning:  symbolic link (%s) failed\n",
              FnFilter1(G.filename)));
            free(slnk_entry);
            errval = CloseError(G.outfile, G.filename);
            return errval ? errval : PK_WARN;
        }
        errval = CloseError(G.outfile, G.filename); /* close "link" file for good... */
        slnk_entry->target[ucsize] = '\0';
        if (QCOND2)
            Info(slide, 0, ((char *)slide, "-> %s ",
              FnFilter1(slnk_entry->target)));
        /* add this symlink record to the list of deferred symlinks */
        if (G.slink_last != NULL)
            G.slink_last->next = slnk_entry;
        else
            G.slink_head = slnk_entry;
        G.slink_last = slnk_entry;
        return errval;
    }
#endif /* SYMLINKS */

#ifdef QLZIP
    if (G.extra_field) {
        static void qlfix OF((__GPRO__ uch *ef_ptr, unsigned ef_len));

        qlfix(__G__ G.extra_field, G.lrec.extra_field_length);
    }
#endif

#if (defined(NO_FCHOWN))
    errval = CloseError(G.outfile, G.filename);
#endif

#ifdef __HAIKU__
    /* handle the BeOS extra field if present */
    if (!uO.J_flag) {
        void *ptr = scanBeOSexfield(G.extra_field,
                                    G.lrec.extra_field_length);

        if (ptr) {
            setBeOSexfield(G.filename, ptr);
#ifdef BEOS_ASSIGN_FILETYPE
        } else {
            /* Otherwise, ask the system to try assigning a MIME type. */
            assign_MIME( G.filename );
#endif
        }
    }
#endif

    /* if -X option was specified and we have UID/GID info, restore it */
    if (have_uidgid_flg
        /* check that both uid and gid values fit into their data sizes */
        && ((ulg)(uid_t)(z_uidgid[0]) == z_uidgid[0])
        && ((ulg)(gid_t)(z_uidgid[1]) == z_uidgid[1])) {
        TTrace((stderr, "close_outfile:  restoring Unix UID/GID info\n"));
#if (defined(NO_FCHOWN))
        if (chown(G.filename, (uid_t)z_uidgid[0], (gid_t)z_uidgid[1]))
#else
        if (fchown(fileno(G.outfile), (uid_t)z_uidgid[0], (gid_t)z_uidgid[1]))
#endif
        {
            if (uO.qflag)
                Info(slide, 0x201, ((char *)slide, CannotSetItemUidGid,
                  z_uidgid[0], z_uidgid[1], FnFilter1(G.filename),
                  strerror(errno)));
            else
                Info(slide, 0x201, ((char *)slide, CannotSetUidGid,
                  z_uidgid[0], z_uidgid[1], strerror(errno)));
        }
    }

#if (!defined(NO_FCHOWN) && defined(NO_FCHMOD))
    errval = CloseError(G.outfile, G.filename);
#endif

#if (!defined(NO_FCHOWN) && !defined(NO_FCHMOD))
/*---------------------------------------------------------------------------
    Change the file permissions from default ones to those stored in the
    zipfile.
  ---------------------------------------------------------------------------*/

    if (fchmod(fileno(G.outfile), filtattr(__G__ G.pInfo->file_attr)))
        perror("fchmod (file attributes) error");

    errval = CloseError(G.outfile, G.filename);
#endif /* !NO_FCHOWN && !NO_FCHMOD */

    /* skip restoring time stamps on user's request */
    if (uO.D_flag <= 1) {
        /* set the file's access and modification times */
        if (utime(G.filename, &(zt.t2))) {
            if (uO.qflag)
                Info(slide, 0x201, ((char *)slide, CannotSetItemTimestamps,
                  FnFilter1(G.filename), strerror(errno)));
            else
                Info(slide, 0x201, ((char *)slide, CannotSetTimestamps,
                  strerror(errno)));
        }
    }

#if (defined(NO_FCHOWN) || defined(NO_FCHMOD))
/*---------------------------------------------------------------------------
    Change the file permissions from default ones to those stored in the
    zipfile.
  ---------------------------------------------------------------------------*/

#ifndef NO_CHMOD
    if (chmod(G.filename, filtattr(__G__ G.pInfo->file_attr)))
        perror("chmod (file attributes) error");
#endif
#endif /* NO_FCHOWN || NO_FCHMOD */

    return errval;
} /* end function close_outfile() */

#endif /* !MTS */


#if (defined(SYMLINKS) && defined(SET_SYMLINK_ATTRIBS))
int set_symlnk_attribs(__G__ slnk_entry)
    __GDEF
    slinkentry *slnk_entry;
{
    if (slnk_entry->attriblen > 0) {
#ifdef __HAIKU__
      if (slnk_entry->attriblen > sizeof(unsigned)) {
        setBeOSexfield(slnk_entry->fname, (uch *)slnk_entry->buf);
      }
#endif
# if (!defined(NO_LCHOWN))
      if (slnk_entry->attriblen > sizeof(unsigned)) {
        ulg *z_uidgid_p = (zvoid *)(slnk_entry->buf + sizeof(unsigned));
        /* check that both uid and gid values fit into their data sizes */
        if (((ulg)(uid_t)(z_uidgid_p[0]) == z_uidgid_p[0]) &&
            ((ulg)(gid_t)(z_uidgid_p[1]) == z_uidgid_p[1])) {
          TTrace((stderr,
            "set_symlnk_attribs:  restoring Unix UID/GID info for\n\
        %s\n",
            FnFilter1(slnk_entry->fname)));
          if (lchown(slnk_entry->fname,
                     (uid_t)z_uidgid_p[0], (gid_t)z_uidgid_p[1]))
          {
            Info(slide, 0x201, ((char *)slide, CannotSetItemUidGid,
              z_uidgid_p[0], z_uidgid_p[1], FnFilter1(slnk_entry->fname),
              strerror(errno)));
          }
        }
      }
# endif /* !NO_LCHOWN */
# if (!defined(NO_LCHMOD))
      TTrace((stderr,
        "set_symlnk_attribs:  restoring Unix attributes for\n        %s\n",
        FnFilter1(slnk_entry->fname)));
      if (lchmod(slnk_entry->fname,
                 filtattr(__G__ *(unsigned *)(zvoid *)slnk_entry->buf)))
          perror("lchmod (file attributes) error");
# endif /* !NO_LCHMOD */
    }
    /* currently, no error propagation... */
    return PK_OK;
} /* end function set_symlnk_attribs() */
#endif /* SYMLINKS && SET_SYMLINK_ATTRIBS */


#ifdef SET_DIR_ATTRIB
/* messages of code for setting directory attributes */
#  ifndef NO_CHMOD
  static ZCONST char DirlistChmodFailed[] =
    "warning:  cannot set permissions for %s\n          %s\n";
#  endif


int defer_dir_attribs(__G__ pd)
    __GDEF
    direntry **pd;
{
    uxdirattr *d_entry;

    d_entry = (uxdirattr *)malloc(sizeof(uxdirattr) + strlen(G.filename));
    *pd = (direntry *)d_entry;
    if (d_entry == (uxdirattr *)NULL) {
        return PK_MEM;
    }
    d_entry->fn = d_entry->fnbuf;
    strcpy(d_entry->fn, G.filename);

    d_entry->perms = G.pInfo->file_attr;

    d_entry->have_uidgid = get_extattribs(__G__ &(d_entry->u.t3),
                                          d_entry->uidgid);
    return PK_OK;
} /* end function defer_dir_attribs() */


int set_direc_attribs(__G__ d)
    __GDEF
    direntry *d;
{
    int errval = PK_OK;

    if (UxAtt(d)->have_uidgid &&
        /* check that both uid and gid values fit into their data sizes */
        ((ulg)(uid_t)(UxAtt(d)->uidgid[0]) == UxAtt(d)->uidgid[0]) &&
        ((ulg)(gid_t)(UxAtt(d)->uidgid[1]) == UxAtt(d)->uidgid[1]) &&
        chown(UxAtt(d)->fn, (uid_t)UxAtt(d)->uidgid[0],
              (gid_t)UxAtt(d)->uidgid[1]))
    {
        Info(slide, 0x201, ((char *)slide, CannotSetItemUidGid,
          UxAtt(d)->uidgid[0], UxAtt(d)->uidgid[1], FnFilter1(d->fn),
          strerror(errno)));
        if (!errval)
            errval = PK_WARN;
    }
    /* Skip restoring directory time stamps on user' request. */
    if (uO.D_flag <= 0) {
        /* restore directory timestamps */
        if (utime(d->fn, &UxAtt(d)->u.t2)) {
            Info(slide, 0x201, ((char *)slide, CannotSetItemTimestamps,
              FnFilter1(d->fn), strerror(errno)));
            if (!errval)
                errval = PK_WARN;
        }
    }
#ifndef NO_CHMOD
    if (chmod(d->fn, UxAtt(d)->perms)) {
        Info(slide, 0x201, ((char *)slide, DirlistChmodFailed,
          FnFilter1(d->fn), strerror(errno)));
        if (!errval)
            errval = PK_WARN;
    }
#endif /* !NO_CHMOD */
    return errval;
} /* end function set_direc_attribs() */

#endif /* SET_DIR_ATTRIB */




#ifdef TIMESTAMP

/***************************/
/*  Function stamp_file()  */
/***************************/

int stamp_file(fname, modtime)
    ZCONST char *fname;
    time_t modtime;
{
    ztimbuf tp;

    tp.modtime = tp.actime = modtime;
    return (utime(fname, &tp));

} /* end function stamp_file() */

#endif /* TIMESTAMP */




#ifndef SFX

/************************/
/*  Function version()  */
/************************/

void version(__G)
    __GDEF
{
#if (defined(__GNUC__) && defined(NX_CURRENT_COMPILER_RELEASE))
    char cc_namebuf[40];
    char cc_versbuf[40];
#else
#if (defined(__SUNPRO_C))
    char cc_versbuf[17];
#else
#if (defined(__HP_cc) || defined(__IBMC__))
    char cc_versbuf[25];
#else
#if (defined(__DECC_VER))
    char cc_versbuf[17];
    int cc_verstyp;
#else
#if (defined(CRAY) && defined(_RELEASE))
    char cc_versbuf[40];
#else
#if defined(__WATCOMC__)
    char buf[80];
#endif /* __WATCOMC__ */
#endif /* (CRAY && _RELEASE) */
#endif /* __DECC_VER */
#endif /* __HP_cc || __IBMC__ */
#endif /* __SUNPRO_C */
#endif /* (__GNUC__ && NX_CURRENT_COMPILER_RELEASE) */

#if ((defined(CRAY) || defined(cray)) && defined(_UNICOS))
    char os_namebuf[40];
#else
#if defined(__NetBSD__)
    char os_namebuf[40];
#endif
#endif

    /* Pyramid, NeXT have problems with huge macro expansion, too:  no Info() */
    sprintf((char *)slide, LoadFarString(CompiledWith),

#ifdef __GNUC__
#  ifdef NX_CURRENT_COMPILER_RELEASE
      (sprintf(cc_namebuf, "NeXT DevKit %d.%02d ",
        NX_CURRENT_COMPILER_RELEASE/100, NX_CURRENT_COMPILER_RELEASE%100),
       cc_namebuf),
      (strlen(__VERSION__) > 8)? "(gcc)" :
        (sprintf(cc_versbuf, "(gcc %s)", __VERSION__), cc_versbuf),
#  elif defined(__clang__)
      "LLVM/clang ", __clang_version__,
#  else
      "gcc ", __VERSION__,
#  endif
#else
#if defined(__SUNPRO_C)
      "Sun C ", (sprintf(cc_versbuf, "version %x", __SUNPRO_C), cc_versbuf),
#else
#if (defined(__HP_cc))
      "HP C ",
      (((__HP_cc% 100) == 0) ?
      (sprintf(cc_versbuf, "version A.%02d.%02d",
      (__HP_cc/ 10000), ((__HP_cc% 10000)/ 100))) :
      (sprintf(cc_versbuf, "version A.%02d.%02d.%02d",
      (__HP_cc/ 10000), ((__HP_cc% 10000)/ 100), (__HP_cc% 100))),
      cc_versbuf),
#else
#if (defined(__DECC_VER))
      "DEC C ",
      (sprintf(cc_versbuf, "%c%d.%d-%03d",
               ((cc_verstyp = (__DECC_VER / 10000) % 10) == 6 ? 'T' :
                (cc_verstyp == 8 ? 'S' : 'V')),
               __DECC_VER / 10000000,
               (__DECC_VER % 10000000) / 100000, __DECC_VER % 1000),
               cc_versbuf),
#else
#if defined(CRAY) && defined(_RELEASE)
      "cc ", (sprintf(cc_versbuf, "version %d", _RELEASE), cc_versbuf),
#else
#ifdef __IBMC__
      "IBM C ",
      (sprintf(cc_versbuf, "version %d.%d.%d",
               (__IBMC__ / 100), ((__IBMC__ / 10) % 10), (__IBMC__ % 10)),
               cc_versbuf),
#else
#if defined(__WATCOMC__)
      "Open Watcom C/C++", (sprintf(buf, " %d.%d", (__WATCOMC__/100) - 11,
                               (__WATCOMC__ % 100) / 10), buf),
#else
#ifdef __VERSION__
#   ifndef IZ_CC_NAME
#    define IZ_CC_NAME "cc "
#   endif
      IZ_CC_NAME, __VERSION__
#else
#   ifndef IZ_CC_NAME
#    define IZ_CC_NAME "cc"
#   endif
      IZ_CC_NAME, "",
#endif /* ?__VERSION__ */
#endif /* ?__WATCOMC__ */
#endif /* ?__IBMC__ */
#endif /* ?(CRAY && _RELEASE) */
#endif /* ?__DECC_VER */
#endif /* ?__HP_cc */
#endif /* ?__SUNPRO_C */
#endif /* ?__GNUC__ */

#ifndef IZ_OS_NAME
#  define IZ_OS_NAME "Unix"
#endif
      IZ_OS_NAME,

#if defined(sgi) || defined(__sgi)
      " (Silicon Graphics IRIX)",
#else
#ifdef sun
#  ifdef sparc
#    ifdef __SVR4
#     ifdef __illumos__
      " (illumos/SPARC)",
#     else
      " (Solaris/SPARC)",
#     endif
#    else /* may or may not be SunOS */
      " (Sun SPARC)",
#    endif
#  else
#  if defined(__amd64__) || defined(__x86_64__)
#    ifdef __illumos__
      " (illumos/x86–64)",
#    else
      " (Solaris/x86–64)",
#    endif
#  else
#  if defined(sun386) || defined(i386)
#    ifdef __SVR4
      " (Solaris/Intel)",
#    else
      " (Sun 386i)",
#    endif
#  else
#  if defined(mc68020) || defined(__mc68020__)
      " (Sun 3)",
#  else /* mc68010 or mc68000:  Sun 2 or earlier */
      " (Sun 2)",
#  endif
#  endif
#  endif
#else
#ifdef __hpux
#  ifdef __ia64
      " (HP-UX on Itanium)",
#  else	  
      " (HP-UX)",
#  endif
#else
#ifdef __osf__
      " (OSF/1)",
#else
#ifdef _AIX
      " (IBM AIX)",
#else
#ifdef aiws
      " (IBM RT/AIX)",
#else
#if defined(CRAY) || defined(cray)
#  ifdef _UNICOS
      (sprintf(os_namebuf, " (Cray UNICOS release %d)", _UNICOS), os_namebuf),
#  else
      " (Cray UNICOS)",
#  endif
#else
#if defined(uts) || defined(UTS)
      " (Amdahl UTS)",
#else
#ifdef NeXT
#  ifdef mc68000
      " (NEXTSTEP/black)",
#  elif defined(hppa)
      " (NEXTSTEP/green)",
#  elif defined(sparc)
      " (NEXTSTEP/yellow)",
#  elif defined(i386)
      " (NEXTSTEP/white)",
#  elif defined(ppc)
      " (Apple Rhapsody/PowerPC)",
#  else
      " (OpenStep)",
#  endif
#else              /* the next dozen or so are somewhat order-dependent */
#ifdef LINUX
#  ifdef __ELF__
      " (Linux ELF)",
#  else
      " (Linux a.out)",
#  endif
#else
#ifdef MINIX
      " (Minix)",
#else
#if defined(SVR5) || defined(UNIXWARE7)
      " (SCO UnixWare 7)",
#else
#ifdef M_UNIX
      " (SCO UNIX)",
#else
#ifdef M_XENIX
      " (SCO Xenix)",
#else
#ifdef __NetBSD__
      " (NetBSD)",
#else
#ifdef __FreeBSD__
      " (FreeBSD)",
#else
#ifdef __OpenBSD__
      " (OpenBSD)",
#else
#ifdef __DragonFly__
      " (DragonFly BSD)",
#else
#ifdef __MidnightBSD__
      " (MidnightBSD)",
#else
#ifdef __bsdi__
      (BSD4_4 == 0.5)? " (BSD/386 1.0)" : " (BSD/386 1.1 or later)",
#else
#ifdef __386BSD__
      (BSD4_4 == 1)? " (386BSD, post-4.4 release)" : " (386BSD)",
#else
#ifdef __CYGWIN__
      " (Cygwin)",
#else
#if defined(i686) || defined(__i686) || defined(__i686__)
      " (Intel 686)",
#else
#if defined(i586) || defined(__i586) || defined(__i586__)
      " (Intel 586)",
#else
#if defined(i486) || defined(__i486) || defined(__i486__)
      " (Intel 486)",
#else
#if defined(i386) || defined(__i386) || defined(__i386__)
      " (Intel 386)",
#else
#ifdef pyr
      " (Pyramid)",
#else
#ifdef ultrix
#  ifdef mips
      " (DEC ULTRIX/MIPS)",
#  else
#  ifdef vax
      " (DEC ULTRIX/VAX)",
#  else /* __alpha? ...this never happened, MAL 5/18/2025 */
      " (DEC/Alpha)",
#  endif
#  endif
#else
#ifdef gould
      " (Gould)",
#else
#ifdef MTS
      " (MTS)",
#else
#ifdef __convexc__
      " (Convex)",
#else
#ifdef __QNX__
      " (QNX 4)",
#else
#ifdef __QNXNTO__
      " (QNX Neutrino)",
#else
#ifdef Lynx
      " (LynxOS)",
#else
#ifdef __APPLE__
#  ifdef __aarch64__
      " (macOS Apple Silicon)",
#  elif defined(__x86_64__)
      " (macOS x86-64)",
#  elif defined(__i386__)
      " (Mac OS X Intel)",
#  elif defined(__ppc__)
      " (Mac OS X PowerPC)",
#  elif defined(__ppc64__)
      " (Mac OS X PowerPC64)",
#  else
      " (A sour Apple)",
#  endif /* __ppc64__ */
#  endif /* __ppc__ */
#  endif /* __i386__ */
#  endif /* __x86_64__ */
#  endif /* __aarch64__ */
#else
#ifdef __HAIKU__
      " (Haiku)",
#else
      "",
#endif /* Haiku */
#endif /* Apple */
#endif /* Lynx */
#endif /* QNX Neutrino */
#endif /* QNX 4 */
#endif /* Convex */
#endif /* MTS */
#endif /* Gould */
#endif /* DEC */
#endif /* Pyramid */
#endif /* 386 */
#endif /* 486 */
#endif /* 586 */
#endif /* 686 */
#endif /* Cygwin */
#endif /* 386BSD */
#endif /* BSDI BSD/386 */
#endif /* MidnightBSD */
#endif /* DragonFly BSD */
#endif /* OpenBSD */
#endif /* FreeBSD */
#endif /* NetBSD */
#endif /* SCO Xenix */
#endif /* SCO Unix */
#endif /* SCO UnixWare 7 */
#endif /* Minix */
#endif /* Linux */
#endif /* NeXT */
#endif /* Amdahl */
#endif /* Cray */
#endif /* RT/AIX */
#endif /* AIX */
#endif /* OSF/1 */
#endif /* HP-UX */
#endif /* Sun */
#endif /* SGI */

#if 0
      " on ", __DATE__
#else
      "", ""
#endif
    );

    (*G.message)((zvoid *)&G, slide, (ulg)strlen((char *)slide), 0);

} /* end function version() */

#endif /* !SFX */




#ifdef QLZIP

struct qdirect  {
    long            d_length __attribute__ ((packed));  /* file length */
    unsigned char   d_access __attribute__ ((packed));  /* file access type */
    unsigned char   d_type __attribute__ ((packed));    /* file type */
    long            d_datalen __attribute__ ((packed)); /* data length */
    long            d_reserved __attribute__ ((packed));/* Unused */
    short           d_szname __attribute__ ((packed));  /* size of name */
    char            d_name[36] __attribute__ ((packed));/* name area */
    long            d_update __attribute__ ((packed));  /* last update */
    long            d_refdate __attribute__ ((packed));
    long            d_backup __attribute__ ((packed));   /* EOD */
};

#define LONGID  "QDOS02"
#define EXTRALEN (sizeof(struct qdirect) + 8)
#define JBLONGID    "QZHD"
#define JBEXTRALEN  (sizeof(jbextra)  - 4 * sizeof(char))

typedef struct {
    char        eb_header[4] __attribute__ ((packed));  /* place_holder */
    char        longid[8] __attribute__ ((packed));
    struct      qdirect     header __attribute__ ((packed));
} qdosextra;

typedef struct {
    char        eb_header[4];                           /* place_holder */
    char        longid[4];
    struct      qdirect     header;
} jbextra;



/*  The following two functions SH() and LG() convert big-endian short
 *  and long numbers into native byte order.  They are some kind of
 *  counterpart to the generic UnZip's makeword() and makelong() functions.
 */
static ush SH(ush val)
{
    uch swapbuf[2];

    swapbuf[1] = (uch)(val & 0xff);
    swapbuf[0] = (uch)(val >> 8);
    return (*(ush *)swapbuf);
}



static ulg LG(ulg val)
{
    /*  convert the big-endian unsigned long number `val' to the machine
     *  dependent representation
     */
    ush swapbuf[2];

    swapbuf[1] = SH((ush)(val & 0xffff));
    swapbuf[0] = SH((ush)(val >> 16));
    return (*(ulg *)swapbuf);
}



static void qlfix(__G__ ef_ptr, ef_len)
    __GDEF
    uch *ef_ptr;
    unsigned ef_len;
{
    while (ef_len >= EB_HEADSIZE)
    {
        unsigned    eb_id  = makeword(EB_ID + ef_ptr);
        unsigned    eb_len = makeword(EB_LEN + ef_ptr);

        if (eb_len > (ef_len - EB_HEADSIZE)) {
            /* discovered some extra field inconsistency! */
            Trace((stderr,
              "qlfix: block length %u > rest ef_size %u\n", eb_len,
              ef_len - EB_HEADSIZE));
            break;
        }

        switch (eb_id) {
          case EF_QDOS:
          {
            struct _ntc_
            {
                long id;
                long dlen;
            } ntc;
            long dlen = 0;

            qdosextra   *extra = (qdosextra *)ef_ptr;
            jbextra     *jbp   = (jbextra   *)ef_ptr;

            if (!strncmp(extra->longid, LONGID, strlen(LONGID)))
            {
                if (eb_len != EXTRALEN)
                    if (uO.qflag)
                        Info(slide, 0x201, ((char *)slide,
                          "warning:  invalid length in Qdos field for %s\n",
                          FnFilter1(G.filename)));
                    else
                        Info(slide, 0x201, ((char *)slide,
                          "warning:  invalid length in Qdos field"));

                if (extra->header.d_type)
                {
                    dlen = extra->header.d_datalen;
                }
            }

            if (!strncmp(jbp->longid, JBLONGID, strlen(JBLONGID)))
            {
                if (eb_len != JBEXTRALEN)
                    if (uO.qflag)
                        Info(slide, 0x201, ((char *)slide,
                          "warning:  invalid length in QZ field for %s\n",
                          FnFilter1(G.filename)));
                    else
                        Info(slide, 0x201, ((char *)slide,
                          "warning:  invalid length in QZ field"));
                if (jbp->header.d_type)
                {
                    dlen = jbp->header.d_datalen;
                }
            }

            if ((long)LG(dlen) > 0)
            {
                zfseeko(G.outfile, -8, SEEK_END);
                fread(&ntc, 8, 1, G.outfile);
                if (ntc.id != *(long *)"XTcc")
                {
                    ntc.id = *(long *)"XTcc";
                    ntc.dlen = dlen;
                    fwrite (&ntc, 8, 1, G.outfile);
                }
                Info(slide, 0x201, ((char *)slide, "QData = %d", LG(dlen)));
            }
            return;     /* finished, cancel further extra field scanning */
          }

          default:
            Trace((stderr,"qlfix: unknown extra field block, ID=%d\n",
               eb_id));
        }

        /* Skip this extra field block */
        ef_ptr += (eb_len + EB_HEADSIZE);
        ef_len -= (eb_len + EB_HEADSIZE);
    }

    // Still not detected? Try to detect by system locale
    if(*OEM_CP == '\0') {

      const char *lcToOemTable[] = {
        "af_ZA", "CP850", "ar_SA", "CP720", "ar_LB", "CP720", "ar_EG", "CP720",
        "ar_DZ", "CP720", "ar_BH", "CP720", "ar_IQ", "CP720", "ar_JO", "CP720",
        "ar_KW", "CP720", "ar_LY", "CP720", "ar_MA", "CP720", "ar_OM", "CP720",
        "ar_QA", "CP720", "ar_SY", "CP720", "ar_TN", "CP720", "ar_AE", "CP720",
        "ar_YE", "CP720", "ast_ES", "CP850", "az_AZ@cyrillic", "CP866", "az_AZ", "CP857",
        "be_BY", "CP866", "bg_BG", "CP866", "br_FR", "CP850", "ca_ES", "CP850",
        "zh_CN", "CP936", "zh_TW", "CP950", "kw_GB", "CP850", "cs_CZ", "CP852",
        "cy_GB", "CP850", "da_DK", "CP850", "de_AT", "CP850", "de_LI", "CP850",
        "de_LU", "CP850", "de_CH", "CP850", "de_DE", "CP850", "el_GR", "CP737",
        "en_AU", "CP850", "en_CA", "CP850", "en_GB", "CP850", "en_IE", "CP850",
        "en_JM", "CP850", "en_BZ", "CP850", "en_PH", "CP437", "en_ZA", "CP437",
        "en_TT", "CP850", "en_US", "CP437", "en_ZW", "CP437", "en_NZ", "CP850",
        "es_PA", "CP850", "es_BO", "CP850", "es_CR", "CP850", "es_DO", "CP850",
        "es_SV", "CP850", "es_EC", "CP850", "es_GT", "CP850", "es_HN", "CP850",
        "es_NI", "CP850", "es_CL", "CP850", "es_MX", "CP850", "es_ES", "CP850",
        "es_CO", "CP850", "es_ES", "CP850", "es_PE", "CP850", "es_AR", "CP850",
        "es_PR", "CP850", "es_VE", "CP850", "es_UY", "CP850", "es_PY", "CP850",
        "et_EE", "CP775", "eu_ES", "CP850", "fa_IR", "CP720", "fi_FI", "CP850",
        "fo_FO", "CP850", "fr_FR", "CP850", "fr_BE", "CP850", "fr_CA", "CP850",
        "fr_LU", "CP850", "fr_MC", "CP850", "fr_CH", "CP850", "ga_IE", "CP437",
        "gd_GB", "CP850", "gv_IM", "CP850", "gl_ES", "CP850", "he_IL", "CP862",
        "hr_HR", "CP852", "hu_HU", "CP852", "id_ID", "CP850", "is_IS", "CP850",
        "it_IT", "CP850", "it_CH", "CP850", "iv_IV", "CP437", "ja_JP", "CP932",
        "kk_KZ", "CP866", "ko_KR", "CP949", "ky_KG", "CP866", "lt_LT", "CP775",
        "lv_LV", "CP775", "mk_MK", "CP866", "mn_MN", "CP866", "ms_BN", "CP850",
        "ms_MY", "CP850", "nl_BE", "CP850", "nl_NL", "CP850", "nl_SR", "CP850",
        "nn_NO", "CP850", "nb_NO", "CP850", "pl_PL", "CP852", "pt_BR", "CP850",
        "pt_PT", "CP850", "rm_CH", "CP850", "ro_RO", "CP852", "ru_RU", "CP866",
        "sk_SK", "CP852", "sl_SI", "CP852", "sq_AL", "CP852", "sr_RS@latin", "CP852",
        "sr_RS", "CP855", "sv_SE", "CP850", "sv_FI", "CP850", "sw_KE", "CP437",
        "th_TH", "CP874", "tr_TR", "CP857", "tt_RU", "CP866", "uk_UA", "CP866",
        "ur_PK", "CP720", "uz_UZ@cyrillic", "CP866", "uz_UZ", "CP857", "vi_VN", "CP1258",
        "wa_BE", "CP850", "zh_HK", "CP950", "zh_SG", "CP936"};

      const char *lcToAnsiTable[] = {
        "af_ZA", "CP1252", "ar_SA", "CP1256", "ar_LB", "CP1256", "ar_EG", "CP1256",
        "ar_DZ", "CP1256", "ar_BH", "CP1256", "ar_IQ", "CP1256", "ar_JO", "CP1256",
        "ar_KW", "CP1256", "ar_LY", "CP1256", "ar_MA", "CP1256", "ar_OM", "CP1256",
        "ar_QA", "CP1256", "ar_SY", "CP1256", "ar_TN", "CP1256", "ar_AE", "CP1256",
        "ar_YE", "CP1256","ast_ES", "CP1252", "az_AZ@cyrillic", "CP1251", "az_AZ", "CP1254",
        "be_BY", "CP1251", "bg_BG", "CP1251", "br_FR", "CP1252", "ca_ES", "CP1252",
        "zh_CN", "CP936",  "zh_TW", "CP950",  "kw_GB", "CP1252", "cs_CZ", "CP1250",
        "cy_GB", "CP1252", "da_DK", "CP1252", "de_AT", "CP1252", "de_LI", "CP1252",
        "de_LU", "CP1252", "de_CH", "CP1252", "de_DE", "CP1252", "el_GR", "CP1253",
        "en_AU", "CP1252", "en_CA", "CP1252", "en_GB", "CP1252", "en_IE", "CP1252",
        "en_JM", "CP1252", "en_BZ", "CP1252", "en_PH", "CP1252", "en_ZA", "CP1252",
        "en_TT", "CP1252", "en_US", "CP1252", "en_ZW", "CP1252", "en_NZ", "CP1252",
        "es_PA", "CP1252", "es_BO", "CP1252", "es_CR", "CP1252", "es_DO", "CP1252",
        "es_SV", "CP1252", "es_EC", "CP1252", "es_GT", "CP1252", "es_HN", "CP1252",
        "es_NI", "CP1252", "es_CL", "CP1252", "es_MX", "CP1252", "es_ES", "CP1252",
        "es_CO", "CP1252", "es_ES", "CP1252", "es_PE", "CP1252", "es_AR", "CP1252",
        "es_PR", "CP1252", "es_VE", "CP1252", "es_UY", "CP1252", "es_PY", "CP1252",
        "et_EE", "CP1257", "eu_ES", "CP1252", "fa_IR", "CP1256", "fi_FI", "CP1252",
        "fo_FO", "CP1252", "fr_FR", "CP1252", "fr_BE", "CP1252", "fr_CA", "CP1252",
        "fr_LU", "CP1252", "fr_MC", "CP1252", "fr_CH", "CP1252", "ga_IE", "CP1252",
        "gd_GB", "CP1252", "gv_IM", "CP1252", "gl_ES", "CP1252", "he_IL", "CP1255",
        "hr_HR", "CP1250", "hu_HU", "CP1250", "id_ID", "CP1252", "is_IS", "CP1252",
        "it_IT", "CP1252", "it_CH", "CP1252", "iv_IV", "CP1252", "ja_JP", "CP932",
        "kk_KZ", "CP1251", "ko_KR", "CP949", "ky_KG", "CP1251", "lt_LT", "CP1257",
        "lv_LV", "CP1257", "mk_MK", "CP1251", "mn_MN", "CP1251", "ms_BN", "CP1252",
        "ms_MY", "CP1252", "nl_BE", "CP1252", "nl_NL", "CP1252", "nl_SR", "CP1252",
        "nn_NO", "CP1252", "nb_NO", "CP1252", "pl_PL", "CP1250", "pt_BR", "CP1252",
        "pt_PT", "CP1252", "rm_CH", "CP1252", "ro_RO", "CP1250", "ru_RU", "CP1251",
        "sk_SK", "CP1250", "sl_SI", "CP1250", "sq_AL", "CP1250", "sr_RS@latin", "CP1250",
        "sr_RS", "CP1251", "sv_SE", "CP1252", "sv_FI", "CP1252", "sw_KE", "CP1252",
        "th_TH", "CP874", "tr_TR", "CP1254", "tt_RU", "CP1251", "uk_UA", "CP1251",
        "ur_PK", "CP1256", "uz_UZ@cyrillic", "CP1251", "uz_UZ", "CP1254", "vi_VN", "CP1258",
        "wa_BE", "CP1252", "zh_HK", "CP950", "zh_SG", "CP936"};

      int tableLen = sizeof(lcToOemTable) / sizeof(lcToOemTable[0]);
      int lcLen = 0, i;

      // Detect required code page name from current locale
      char *lc = setlocale(LC_CTYPE, "");

      if (lc && lc[0]) {
        // Compare up to the dot, if it exists, e.g. en_US.UTF-8
        for (lcLen = 0; lc[lcLen] != '.' && lc[lcLen] != ':' && lc[lcLen] != '\0'; ++lcLen);

        for (i = 0; i < tableLen; i += 2)

          if (strncmp(lc, (lcToOemTable[i]), lcLen) == 0) {

            strncpy(OEM_CP, lcToOemTable[i + 1],
              sizeof(OEM_CP));

            strncpy(ANSI_CP, lcToAnsiTable[i + 1],
              sizeof(ANSI_CP));

            break;
          }
      }
    }
}
#endif /* QLZIP */

#ifdef __HAIKU__

/******************************/
/* Extra field functions      */
/******************************/

/*
** Scan the extra fields in extra_field, and look for a BeOS EF; return a
** pointer to that EF, or NULL if it's not there.
*/
static uch *scanBeOSexfield(const uch *ef_ptr, unsigned ef_len)
{
    while( ef_ptr != NULL && ef_len >= EB_HEADSIZE ) {
        unsigned eb_id  = makeword(EB_ID + ef_ptr);
        unsigned eb_len = makeword(EB_LEN + ef_ptr);

        if (eb_len > (ef_len - EB_HEADSIZE)) {
            Trace((stderr,
              "scanBeOSexfield: block length %u > rest ef_size %u\n", eb_len,
              ef_len - EB_HEADSIZE));
            break;
        }

        if (eb_id == EF_BEOS && eb_len >= EB_BEOS_HLEN) {
            return (uch *)ef_ptr;
        }

        ef_ptr += (eb_len + EB_HEADSIZE);
        ef_len -= (eb_len + EB_HEADSIZE);
    }

    return NULL;
}

/* Used by setBeOSexfield():

Set a file/directory's attributes to the attributes passed in.

If set_file_attrs() fails, an error will be returned:

     EOK - no errors occurred

(other values will be whatever the failed function returned; no docs
yet, or I'd list a few)
*/
static int set_file_attrs( const char *name,
                           const unsigned char *attr_buff,
                           const off_t total_attr_size )
{
    int                  retval = EOK;
    unsigned char       *ptr;
    const unsigned char *guard;
    int                  fd;

    ptr   = (unsigned char *)attr_buff;
    guard = ptr + total_attr_size;

    fd = open(name, O_RDONLY | O_NOTRAVERSE);
    if (fd < 0) {
        return errno; /* should it be -fd ? */
    }

    while (ptr < guard) {
        ssize_t              wrote_bytes;
        const char          *attr_name;
        unsigned char       *attr_data;
        uint32               attr_type;
        int64                attr_size;

        attr_name  = (char *)&(ptr[0]);
        ptr       += strlen(attr_name) + 1;

        /* The attr_info data is stored in big-endian format because the */
        /* PowerPC port was here first.                                  */
        memcpy(&attr_type, ptr, 4); ptr += 4;
        memcpy(&attr_size, ptr, 8); ptr += 8;
        attr_type = (uint32)B_BENDIAN_TO_HOST_INT32( attr_type );
        attr_size = (off_t)B_BENDIAN_TO_HOST_INT64( attr_size );

        if (attr_size < 0LL) {
            Info(slide, 0x201, ((char *)slide,
                 "warning: skipping attribute with invalid length (%Ld)\n",
                 attr_size));
            break;
        }

        attr_data  = ptr;
        ptr       += attr_size;

        if (ptr > guard) {
            /* We've got a truncated attribute. */
            Info(slide, 0x201, ((char *)slide,
                 "warning: truncated attribute\n"));
            break;
        }

        /* Wave the magic wand... this will swap Be-known types properly. */
        (void)swap_data( attr_type, attr_data, attr_size,
                         B_SWAP_BENDIAN_TO_HOST );

        wrote_bytes = fs_write_attr(fd, attr_name, attr_type, 0,
                                    attr_data, attr_size);
        if (wrote_bytes != attr_size) {
            Info(slide, 0x201, ((char *)slide,
                 "warning: wrote %ld attribute bytes of %ld\n",
                 (unsigned long)wrote_bytes,(unsigned long)attr_size));
        }
    }

    close(fd);

    return retval;
}

static void setBeOSexfield(const char *path, uch *extra_field)
{
    uch *ptr       = extra_field;
    ush  id        = 0;
    ush  size      = 0;
    ulg  full_size = 0;
    uch  flags     = 0;
    uch *attrbuff  = NULL;
    int retval;

    if( extra_field == NULL ) {
        return;
    }

    /* Collect the data from the extra field buffer. */
    id        = makeword(ptr);    ptr += 2;   /* we don't use this... */
    size      = makeword(ptr);    ptr += 2;
    full_size = makelong(ptr);    ptr += 4;
    flags     = *ptr;             ptr++;

    /* Do a little sanity checking. */
    if (flags & EB_BE_FL_BADBITS) {
        /* corrupted or unsupported */
        Info(slide, 0x201, ((char *)slide,
          "Unsupported flags set for this BeOS extra field, skipping.\n"));
        return;
    }
    if (size <= EB_BEOS_HLEN) {
        /* corrupted, unsupported, or truncated */
        Info(slide, 0x201, ((char *)slide,
             "BeOS extra field is %d bytes, should be at least %d.\n", size,
             EB_BEOS_HLEN));
        return;
    }
    if (full_size < (size - EB_BEOS_HLEN)) {
        /* possible old archive? will this screw up on valid archives? */
        Info(slide, 0x201, ((char *)slide,
             "Skipping attributes: BeOS extra field is %d bytes, "
             "data size is %ld.\n", size - EB_BEOS_HLEN, full_size));
        return;
    }

    /* Find the BeOS file attribute data. */
    if (flags & EB_BE_FL_UNCMPR) {
        /* Uncompressed data */
        attrbuff = ptr;
    } else {
        /* Compressed data */
        attrbuff = (uch *)malloc( full_size );
        if (attrbuff == NULL) {
            /* No memory to uncompress attributes */
            Info(slide, 0x201, ((char *)slide,
                 "Can't allocate memory to uncompress file attributes.\n"));
            return;
        }

        retval = memextract(__G__ attrbuff, full_size,
                            ptr, size - EB_BEOS_HLEN);
        if( retval != PK_OK ) {
            /* error uncompressing attributes */
            Info(slide, 0x201, ((char *)slide,
                 "Error uncompressing file attributes.\n"));

            /* Some errors here might not be so bad; we should expect */
            /* some truncated data, for example.  If the data was     */
            /* corrupt, we should _not_ attempt to restore the attrs  */
            /* for this file... there's no way to detect what attrs   */
            /* are good and which are bad.                            */
            free (attrbuff);
            return;
        }
    }

    /* Now attempt to set the file attributes on the extracted file. */
    retval = set_file_attrs(path, attrbuff, (off_t)full_size);
    if (retval != EOK) {
        Info(slide, 0x201, ((char *)slide,
             "Error writing file attributes.\n"));
    }

    /* Clean up, if necessary */
    if (attrbuff != ptr) {
        free(attrbuff);
    }

    return;
}

#ifdef BEOS_USE_PRINTEXFIELD
static void printBeOSexfield( int isdir, uch *extra_field )
{
    uch *ptr       = extra_field;
    ush  id        = 0;
    ush  size      = 0;
    ulg  full_size = 0;
    uch  flags     = 0;

    /* Tell picky compilers to be quiet. */
    isdir = isdir;

    if( extra_field == NULL ) {
        return;
    }

    /* Collect the data from the buffer. */
    id        = makeword( ptr );    ptr += 2;
    size      = makeword( ptr );    ptr += 2;
    full_size = makelong( ptr );    ptr += 4;
    flags     = *ptr;               ptr++;

    if( id != EF_BEOS ) {
        /* not a 'Be' field */
        printf("\t*** Unknown field type (0x%04x, '%c%c')\n", id,
               (char)(id >> 8), (char)id);
    }

    if( flags & EB_BE_FL_BADBITS ) {
        /* corrupted or unsupported */
        printf("\t*** Corrupted BeOS extra field:\n");
        printf("\t*** unknown bits set in the flags\n");
        printf("\t*** (Possibly created by an old version of zip for BeOS.\n");
    }

    if( size <= EB_BEOS_HLEN ) {
        /* corrupted, unsupported, or truncated */
        printf("\t*** Corrupted BeOS extra field:\n");
        printf("\t*** size is %d, should be larger than %d\n", size,
               EB_BEOS_HLEN );
    }

    if( flags & EB_BE_FL_UNCMPR ) {
        /* Uncompressed data */
        printf("\tBeOS extra field data (uncompressed):\n");
        printf("\t\t%ld data bytes\n", full_size);
    } else {
        /* Compressed data */
        printf("\tBeOS extra field data (compressed):\n");
        printf("\t\t%d compressed bytes\n", size - EB_BEOS_HLEN);
        printf("\t\t%ld uncompressed bytes\n", full_size);
    }
}
#endif

#ifdef BEOS_ASSIGN_FILETYPE
/* Note: This will no longer be necessary in BeOS PR4; update_mime_info()    */
/* will be updated to build its own absolute pathname if it's not given one. */
static void assign_MIME( const char *file )
{
    char *fullname;
    char buff[PATH_MAX], cwd_buff[PATH_MAX];
    int retval;

    if( file[0] == '/' ) {
        fullname = (char *)file;
    } else {
        sprintf( buff, "%s/%s", getcwd( cwd_buff, PATH_MAX ), file );
        fullname = buff;
    }

    retval = update_mime_info( fullname, FALSE, TRUE, TRUE );
}
#endif

#endif

typedef struct {
    char *local_charset;
    char *archive_charset;
} CHARSET_MAP;

/* A mapping of local <-> archive charsets used by default to convert filenames
 * of DOS/Windows Zip archives. Currently very basic. */
static CHARSET_MAP dos_charset_map[] = {
    { "ANSI_X3.4-1968", "CP850" },
    { "ISO-8859-1", "CP850" },
    { "CP1252", "CP850" },
    { "KOI8-R", "CP866" },
    { "KOI8-U", "CP866" },
    { "ISO-8859-5", "CP866" }
};

char OEM_CP[MAX_CP_NAME] = "";
char ISO_CP[MAX_CP_NAME] = "";
char ANSI_CP[MAX_CP_NAME] = "";
int forcedCP = 0;

/* Try to guess the default value of OEM_CP based on the current locale.
 * ISO_CP is left alone for now. */
void init_conversion_charsets()
{
    const char *local_charset;
    int i;

    /* Make a guess only if OEM_CP not already set. */
    if(*OEM_CP == '\0') {
    	local_charset = nl_langinfo(CODESET);
    	for(i = 0; i < sizeof(dos_charset_map)/sizeof(CHARSET_MAP); i++)
    		if(!strcasecmp(local_charset, dos_charset_map[i].local_charset)) {
    			strncpy(OEM_CP, dos_charset_map[i].archive_charset,
    					sizeof(OEM_CP));
    			break;
    		}
    }

    // Still not detected? Try to detect by system locale
    if(*OEM_CP == '\0') {

      const char *lcToOemTable[] = {
        "af_ZA", "CP850", "ar_SA", "CP720", "ar_LB", "CP720", "ar_EG", "CP720",
        "ar_DZ", "CP720", "ar_BH", "CP720", "ar_IQ", "CP720", "ar_JO", "CP720",
        "ar_KW", "CP720", "ar_LY", "CP720", "ar_MA", "CP720", "ar_OM", "CP720",
        "ar_QA", "CP720", "ar_SY", "CP720", "ar_TN", "CP720", "ar_AE", "CP720",
        "ar_YE", "CP720", "ast_ES", "CP850", "az_AZ@cyrillic", "CP866", "az_AZ", "CP857",
        "be_BY", "CP866", "bg_BG", "CP866", "br_FR", "CP850", "ca_ES", "CP850",
        "zh_CN", "CP936", "zh_TW", "CP950", "kw_GB", "CP850", "cs_CZ", "CP852",
        "cy_GB", "CP850", "da_DK", "CP850", "de_AT", "CP850", "de_LI", "CP850",
        "de_LU", "CP850", "de_CH", "CP850", "de_DE", "CP850", "el_GR", "CP737",
        "en_AU", "CP850", "en_CA", "CP850", "en_GB", "CP850", "en_IE", "CP850",
        "en_JM", "CP850", "en_BZ", "CP850", "en_PH", "CP437", "en_ZA", "CP437",
        "en_TT", "CP850", "en_US", "CP437", "en_ZW", "CP437", "en_NZ", "CP850",
        "es_PA", "CP850", "es_BO", "CP850", "es_CR", "CP850", "es_DO", "CP850",
        "es_SV", "CP850", "es_EC", "CP850", "es_GT", "CP850", "es_HN", "CP850",
        "es_NI", "CP850", "es_CL", "CP850", "es_MX", "CP850", "es_ES", "CP850",
        "es_CO", "CP850", "es_ES", "CP850", "es_PE", "CP850", "es_AR", "CP850",
        "es_PR", "CP850", "es_VE", "CP850", "es_UY", "CP850", "es_PY", "CP850",
        "et_EE", "CP775", "eu_ES", "CP850", "fa_IR", "CP720", "fi_FI", "CP850",
        "fo_FO", "CP850", "fr_FR", "CP850", "fr_BE", "CP850", "fr_CA", "CP850",
        "fr_LU", "CP850", "fr_MC", "CP850", "fr_CH", "CP850", "ga_IE", "CP437",
        "gd_GB", "CP850", "gv_IM", "CP850", "gl_ES", "CP850", "he_IL", "CP862",
        "hr_HR", "CP852", "hu_HU", "CP852", "id_ID", "CP850", "is_IS", "CP850",
        "it_IT", "CP850", "it_CH", "CP850", "iv_IV", "CP437", "ja_JP", "CP932",
        "kk_KZ", "CP866", "ko_KR", "CP949", "ky_KG", "CP866", "lt_LT", "CP775",
        "lv_LV", "CP775", "mk_MK", "CP866", "mn_MN", "CP866", "ms_BN", "CP850",
        "ms_MY", "CP850", "nl_BE", "CP850", "nl_NL", "CP850", "nl_SR", "CP850",
        "nn_NO", "CP850", "nb_NO", "CP850", "pl_PL", "CP852", "pt_BR", "CP850",
        "pt_PT", "CP850", "rm_CH", "CP850", "ro_RO", "CP852", "ru_RU", "CP866",
        "sk_SK", "CP852", "sl_SI", "CP852", "sq_AL", "CP852", "sr_RS@latin", "CP852",
        "sr_RS", "CP855", "sv_SE", "CP850", "sv_FI", "CP850", "sw_KE", "CP437",
        "th_TH", "CP874", "tr_TR", "CP857", "tt_RU", "CP866", "uk_UA", "CP866",
        "ur_PK", "CP720", "uz_UZ@cyrillic", "CP866", "uz_UZ", "CP857", "vi_VN", "CP1258",
        "wa_BE", "CP850", "zh_HK", "CP950", "zh_SG", "CP936"};

      const char *lcToAnsiTable[] = {
        "af_ZA", "CP1252", "ar_SA", "CP1256", "ar_LB", "CP1256", "ar_EG", "CP1256",
        "ar_DZ", "CP1256", "ar_BH", "CP1256", "ar_IQ", "CP1256", "ar_JO", "CP1256",
        "ar_KW", "CP1256", "ar_LY", "CP1256", "ar_MA", "CP1256", "ar_OM", "CP1256",
        "ar_QA", "CP1256", "ar_SY", "CP1256", "ar_TN", "CP1256", "ar_AE", "CP1256",
        "ar_YE", "CP1256","ast_ES", "CP1252", "az_AZ@cyrillic", "CP1251", "az_AZ", "CP1254",
        "be_BY", "CP1251", "bg_BG", "CP1251", "br_FR", "CP1252", "ca_ES", "CP1252",
        "zh_CN", "CP936",  "zh_TW", "CP950",  "kw_GB", "CP1252", "cs_CZ", "CP1250",
        "cy_GB", "CP1252", "da_DK", "CP1252", "de_AT", "CP1252", "de_LI", "CP1252",
        "de_LU", "CP1252", "de_CH", "CP1252", "de_DE", "CP1252", "el_GR", "CP1253",
        "en_AU", "CP1252", "en_CA", "CP1252", "en_GB", "CP1252", "en_IE", "CP1252",
        "en_JM", "CP1252", "en_BZ", "CP1252", "en_PH", "CP1252", "en_ZA", "CP1252",
        "en_TT", "CP1252", "en_US", "CP1252", "en_ZW", "CP1252", "en_NZ", "CP1252",
        "es_PA", "CP1252", "es_BO", "CP1252", "es_CR", "CP1252", "es_DO", "CP1252",
        "es_SV", "CP1252", "es_EC", "CP1252", "es_GT", "CP1252", "es_HN", "CP1252",
        "es_NI", "CP1252", "es_CL", "CP1252", "es_MX", "CP1252", "es_ES", "CP1252",
        "es_CO", "CP1252", "es_ES", "CP1252", "es_PE", "CP1252", "es_AR", "CP1252",
        "es_PR", "CP1252", "es_VE", "CP1252", "es_UY", "CP1252", "es_PY", "CP1252",
        "et_EE", "CP1257", "eu_ES", "CP1252", "fa_IR", "CP1256", "fi_FI", "CP1252",
        "fo_FO", "CP1252", "fr_FR", "CP1252", "fr_BE", "CP1252", "fr_CA", "CP1252",
        "fr_LU", "CP1252", "fr_MC", "CP1252", "fr_CH", "CP1252", "ga_IE", "CP1252",
        "gd_GB", "CP1252", "gv_IM", "CP1252", "gl_ES", "CP1252", "he_IL", "CP1255",
        "hr_HR", "CP1250", "hu_HU", "CP1250", "id_ID", "CP1252", "is_IS", "CP1252",
        "it_IT", "CP1252", "it_CH", "CP1252", "iv_IV", "CP1252", "ja_JP", "CP932",
        "kk_KZ", "CP1251", "ko_KR", "CP949", "ky_KG", "CP1251", "lt_LT", "CP1257",
        "lv_LV", "CP1257", "mk_MK", "CP1251", "mn_MN", "CP1251", "ms_BN", "CP1252",
        "ms_MY", "CP1252", "nl_BE", "CP1252", "nl_NL", "CP1252", "nl_SR", "CP1252",
        "nn_NO", "CP1252", "nb_NO", "CP1252", "pl_PL", "CP1250", "pt_BR", "CP1252",
        "pt_PT", "CP1252", "rm_CH", "CP1252", "ro_RO", "CP1250", "ru_RU", "CP1251",
        "sk_SK", "CP1250", "sl_SI", "CP1250", "sq_AL", "CP1250", "sr_RS@latin", "CP1250",
        "sr_RS", "CP1251", "sv_SE", "CP1252", "sv_FI", "CP1252", "sw_KE", "CP1252",
        "th_TH", "CP874", "tr_TR", "CP1254", "tt_RU", "CP1251", "uk_UA", "CP1251",
        "ur_PK", "CP1256", "uz_UZ@cyrillic", "CP1251", "uz_UZ", "CP1254", "vi_VN", "CP1258",
        "wa_BE", "CP1252", "zh_HK", "CP950", "zh_SG", "CP936"};

      int tableLen = sizeof(lcToOemTable) / sizeof(lcToOemTable[0]);
      int lcLen = 0, i;

      // Detect required code page name from current locale
      char *lc = setlocale(LC_CTYPE, "");

      if (lc && lc[0]) {
        // Compare up to the dot, if it exists, e.g. en_US.UTF-8
        for (lcLen = 0; lc[lcLen] != '.' && lc[lcLen] != ':' && lc[lcLen] != '\0'; ++lcLen);

        for (i = 0; i < tableLen; i += 2)

          if (strncmp(lc, (lcToOemTable[i]), lcLen) == 0) {

            strncpy(OEM_CP, lcToOemTable[i + 1],
              sizeof(OEM_CP));

            strncpy(ANSI_CP, lcToAnsiTable[i + 1],
              sizeof(ANSI_CP));

            break;
          }
      }
    }
}

/* Convert a string from one encoding to the current locale using iconv().
 * Be as non-intrusive as possible. If error is encountered during covertion
 * just leave the string intact. */
static void charset_to_intern(char *string, char *from_charset)
{
    iconv_t cd;
    char *s,*d, *buf;
    size_t slen, dlen, buflen;
    const char *local_charset;

    if(*from_charset == '\0')
    	return;

    buf = NULL;
    local_charset = nl_langinfo(CODESET);

    if((cd = iconv_open(local_charset, from_charset)) == (iconv_t)-1)
        return;

    slen = strlen(string);
    s = string;

    /*  Make sure OUTBUFSIZ + 1 never ends up smaller than FILNAMSIZ
     *  as this function also gets called with G.outbuf in fileio.c
     */
    buflen = FILNAMSIZ;
    if (OUTBUFSIZ + 1 < FILNAMSIZ)
    {
        buflen = OUTBUFSIZ + 1;
    }

    d = buf = malloc(buflen);
    if(!d)
    	goto cleanup;

    bzero(buf,buflen);
    dlen = buflen - 1;

    if(iconv(cd, &s, &slen, &d, &dlen) == (size_t)-1)
    	goto cleanup;
    strncpy(string, buf, buflen);

    cleanup:
    free(buf);
    iconv_close(cd);
}

/* Convert a string from OEM_CP to the current locale charset. */
inline void oem_intern(char *string)
{
    charset_to_intern(string, OEM_CP);
}

/* Convert a string from ISO_CP to the current locale charset. */
inline void iso_intern(char *string)
{
    charset_to_intern(string, ISO_CP);
}

/* Convert a string from ANSI_CP to the current locale charset. */
inline void ansi_intern(char *string)
{
    charset_to_intern(string, ANSI_CP);
}
