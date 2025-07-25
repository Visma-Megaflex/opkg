/* vi: set expandtab sw=4 sts=4: */
/* pkg.c - the opkg package management system

   Carl D. Worth

   Copyright (C) 2001 University of Southern California

   SPDX-License-Identifier: GPL-2.0-or-later

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "pkg.h"

#include "pkg_parse.h"
#include "pkg_extract.h"
#include "opkg_download.h"
#include "opkg_message.h"
#include "opkg_utils.h"
#include "opkg_verify.h"

#include "xfuncs.h"
#include "sprintf_alloc.h"
#include "file_util.h"
#include "xsystem.h"
#include "opkg_conf.h"

typedef struct enum_map enum_map_t;
struct enum_map {
    unsigned int value;
    const char *str;
};

static const enum_map_t pkg_state_want_map[] = {
    {SW_UNKNOWN, "unknown"},
    {SW_INSTALL, "install"},
    {SW_DEINSTALL, "deinstall"},
    {SW_PURGE, "purge"}
};

static const enum_map_t pkg_state_flag_map[] = {
    {SF_OK, "ok"},
    {SF_REINSTREQ, "reinstreq"},
    {SF_HOLD, "hold"},
    {SF_REPLACE, "replace"},
    {SF_NOPRUNE, "noprune"},
    {SF_PREFER, "prefer"},
    {SF_OBSOLETE, "obsolete"},
    {SF_USER, "user"},
};

static const enum_map_t pkg_state_status_map[] = {
    {SS_NOT_INSTALLED, "not-installed"},
    {SS_UNPACKED, "unpacked"},
    {SS_HALF_CONFIGURED, "half-configured"},
    {SS_INSTALLED, "installed"},
    {SS_HALF_INSTALLED, "half-installed"},
    {SS_CONFIG_FILES, "config-files"},
    {SS_POST_INST_FAILED, "post-inst-failed"},
    {SS_REMOVAL_FAILED, "removal-failed"}
};

static void pkg_init(pkg_t * pkg)
{
    pkg->name = NULL;
    pkg->epoch = 0;
    pkg->version = NULL;
    pkg->revision = NULL;
    pkg->force_reinstall = 0;
    pkg->dest = NULL;
    pkg->src = NULL;
    pkg->architecture = NULL;
    pkg->maintainer = NULL;
    pkg->section = NULL;
    pkg->description = NULL;
    pkg->tags = NULL;
    pkg->state_want = SW_UNKNOWN;
    pkg->wanted_by = pkg_vec_alloc();
    pkg->state_flag = SF_OK;
    pkg->state_status = SS_NOT_INSTALLED;
    pkg->depends_str = NULL;
    pkg->provides_str = NULL;
    pkg->depends_count = 0;
    pkg->depends = NULL;
    pkg->suggests_str = NULL;
    pkg->recommends_str = NULL;
    pkg->suggests_count = 0;
    pkg->recommends_count = 0;

    pkg->conflicts = NULL;
    pkg->conflicts_count = 0;

    pkg->replaces = NULL;
    pkg->replaces_count = 0;

    pkg->pre_depends_count = 0;
    pkg->pre_depends_str = NULL;
    pkg->provides_count = 0;
    pkg->provides = NULL;
    pkg->filename = NULL;
    pkg->local_filename = NULL;
    pkg->tmp_unpack_dir = NULL;
    pkg->md5sum = NULL;
    pkg->sha256sum = NULL;
    pkg->size = 0;
    pkg->installed_size = 0;
    pkg->priority = NULL;
    pkg->source = NULL;
    conffile_list_init(&pkg->conffiles);
    pkg->installed_files = NULL;
    pkg->installed_files_ref_cnt = 0;
    pkg->essential = 0;
    pkg->provided_by_hand = 0;

    if (opkg_config->verbose_status_file) {
        nv_pair_list_init(&pkg->userfields);
    }
}

pkg_t *pkg_new(void)
{
    pkg_t *pkg;

    pkg = xcalloc(1, sizeof(pkg_t));
    pkg_init(pkg);

    return pkg;
}

static void compound_depend_deinit(compound_depend_t * depends)
{
    int i;
    for (i = 0; i < depends->possibility_count; i++) {
        depend_t *d;
        d = depends->possibilities[i];
        free(d->version);
        free(d);
    }
    free(depends->possibilities);
}

void pkg_deinit(pkg_t * pkg)
{
    unsigned int i;

    free(pkg->name);
    pkg->name = NULL;

    pkg->epoch = 0;

    free(pkg->version);
    pkg->version = NULL;
    /* revision shares storage with version, so don't free */
    pkg->revision = NULL;

    pkg->force_reinstall = 0;

    /* owned by opkg_conf_t */
    pkg->dest = NULL;
    /* owned by opkg_conf_t */
    pkg->src = NULL;

    free(pkg->architecture);
    pkg->architecture = NULL;

    free(pkg->maintainer);
    pkg->maintainer = NULL;

    free(pkg->section);
    pkg->section = NULL;

    free(pkg->description);
    pkg->description = NULL;

    pkg->state_want = SW_UNKNOWN;
    pkg_vec_free(pkg->wanted_by);
    pkg->state_flag = SF_OK;
    pkg->state_status = SS_NOT_INSTALLED;

    if (pkg->replaces) {
        for (i = 0; i < pkg->replaces_count; i++)
            compound_depend_deinit(&pkg->replaces[i]);
        free(pkg->replaces);
    }

    if (pkg->depends) {
        unsigned int count = pkg->pre_depends_count + pkg->depends_count
            + pkg->recommends_count + pkg->suggests_count;

        for (i = 0; i < count; i++)
            compound_depend_deinit(&pkg->depends[i]);
        free(pkg->depends);
    }

    if (pkg->conflicts) {
        for (i = 0; i < pkg->conflicts_count; i++)
            compound_depend_deinit(&pkg->conflicts[i]);
        free(pkg->conflicts);
    }

    free(pkg->provides);

    pkg->pre_depends_count = 0;
    pkg->provides_count = 0;

    free(pkg->filename);
    pkg->filename = NULL;

    free(pkg->local_filename);
    pkg->local_filename = NULL;

    /* CLEANUP: It'd be nice to pullin the cleanup function from
     * opkg_install.c here. See comment in
     * opkg_install.c:cleanup_temporary_files */
    free(pkg->tmp_unpack_dir);
    pkg->tmp_unpack_dir = NULL;

    free(pkg->md5sum);
    pkg->md5sum = NULL;

    free(pkg->sha256sum);
    pkg->sha256sum = NULL;

    free(pkg->priority);
    pkg->priority = NULL;

    free(pkg->source);
    pkg->source = NULL;

    conffile_list_deinit(&pkg->conffiles);

    if (opkg_config->verbose_status_file)
        nv_pair_list_deinit(&pkg->userfields);

    /* XXX: QUESTION: Is forcing this to 1 correct? I suppose so,
     * since if they are calling deinit, they should know. Maybe do an
     * assertion here instead? */
    pkg->installed_files_ref_cnt = 1;
    pkg_free_installed_files(pkg);
    pkg->essential = 0;

    free(pkg->tags);
    pkg->tags = NULL;
}

int pkg_init_from_file(pkg_t * pkg, const char *filename)
{
    int fd, err = 0;
    FILE *control_file;
    char *control_path, *tmp;

    pkg_init(pkg);

    pkg->local_filename = xstrdup(filename);

    tmp = xstrdup(filename);
    sprintf_alloc(&control_path, "%s/%s.control.XXXXXX", opkg_config->tmp_dir,
                  basename(tmp));
    free(tmp);
    fd = mkstemp(control_path);
    if (fd == -1) {
        opkg_perror(ERROR, "Failed to make temp file %s", control_path);
        err = -1;
        goto err0;
    }

    control_file = fdopen(fd, "r+");
    if (control_file == NULL) {
        opkg_perror(ERROR, "Failed to fdopen %s", control_path);
        close(fd);
        err = -1;
        goto err1;
    }

    err = pkg_extract_control_file_to_stream(pkg, control_file);
    if (err) {
        opkg_msg(ERROR, "Failed to extract control file from %s.\n", filename);
        goto err2;
    }

    rewind(control_file);

    err = pkg_parse_from_stream(pkg, control_file, 0);
    if (err) {
        if (err == 1) {
            opkg_msg(ERROR, "Malformed package file %s.\n", filename);
        }
        err = -1;
    }

 err2:
    fclose(control_file);
 err1:
    unlink(control_path);
 err0:
    free(control_path);

    return err;
}

/* Merge any new information in newpkg into oldpkg */
int pkg_merge(pkg_t * oldpkg, pkg_t * newpkg)
{
    if (oldpkg == newpkg) {
        return 0;
    }

    if (!oldpkg->auto_installed)
        oldpkg->auto_installed = newpkg->auto_installed;

    if (!oldpkg->src)
        oldpkg->src = newpkg->src;
    if (!oldpkg->dest)
        oldpkg->dest = newpkg->dest;
    if (!oldpkg->architecture)
        oldpkg->architecture = xstrdup(newpkg->architecture);
    if (!oldpkg->arch_priority)
        oldpkg->arch_priority = newpkg->arch_priority;
    if (!oldpkg->section)
        oldpkg->section = xstrdup(newpkg->section);
    if (!oldpkg->maintainer)
        oldpkg->maintainer = xstrdup(newpkg->maintainer);
    if (!oldpkg->description)
        oldpkg->description = xstrdup(newpkg->description);

    if (!oldpkg->depends_count && !oldpkg->pre_depends_count
        && !oldpkg->recommends_count && !oldpkg->suggests_count) {
        oldpkg->depends_count = newpkg->depends_count;
        newpkg->depends_count = 0;

        oldpkg->depends = newpkg->depends;
        newpkg->depends = NULL;

        oldpkg->pre_depends_count = newpkg->pre_depends_count;
        newpkg->pre_depends_count = 0;

        oldpkg->recommends_count = newpkg->recommends_count;
        newpkg->recommends_count = 0;

        oldpkg->suggests_count = newpkg->suggests_count;
        newpkg->suggests_count = 0;
    }

    if (oldpkg->provides_count <= 1) {
        oldpkg->provides_count = newpkg->provides_count;
        newpkg->provides_count = 0;

        free(oldpkg->provides);
        oldpkg->provides = newpkg->provides;
        newpkg->provides = NULL;
    }

    if (!oldpkg->conflicts_count) {
        oldpkg->conflicts_count = newpkg->conflicts_count;
        newpkg->conflicts_count = 0;

        oldpkg->conflicts = newpkg->conflicts;
        newpkg->conflicts = NULL;
    }

    if (!oldpkg->replaces_count) {
        oldpkg->replaces_count = newpkg->replaces_count;
        newpkg->replaces_count = 0;

        oldpkg->replaces = newpkg->replaces;
        newpkg->replaces = NULL;
    }

    if (!oldpkg->filename)
        oldpkg->filename = xstrdup(newpkg->filename);
    if (!oldpkg->local_filename)
        oldpkg->local_filename = xstrdup(newpkg->local_filename);
    if (!oldpkg->tmp_unpack_dir)
        oldpkg->tmp_unpack_dir = xstrdup(newpkg->tmp_unpack_dir);
    if (!oldpkg->md5sum)
        oldpkg->md5sum = xstrdup(newpkg->md5sum);
    if (!oldpkg->sha256sum)
        oldpkg->sha256sum = xstrdup(newpkg->sha256sum);
    if (!oldpkg->size)
        oldpkg->size = newpkg->size;
    if (!oldpkg->installed_size)
        oldpkg->installed_size = newpkg->installed_size;
    if (!oldpkg->priority)
        oldpkg->priority = xstrdup(newpkg->priority);

    if (opkg_config->verbose_status_file) {
        if (nv_pair_list_empty(&oldpkg->userfields)) {
            list_splice_init(&newpkg->userfields.head, &oldpkg->userfields.head);
        }
    }

    if (!oldpkg->source)
        oldpkg->source = xstrdup(newpkg->source);

    if (nv_pair_list_empty(&oldpkg->conffiles)) {
        list_splice_init(&newpkg->conffiles.head, &oldpkg->conffiles.head);
    }

    if (!oldpkg->installed_files) {
        oldpkg->installed_files = newpkg->installed_files;
        oldpkg->installed_files_ref_cnt = newpkg->installed_files_ref_cnt;
        newpkg->installed_files = NULL;
    }

    if (!oldpkg->essential)
        oldpkg->essential = newpkg->essential;

    return 0;
}

static void abstract_pkg_init(abstract_pkg_t * ab_pkg)
{
    ab_pkg->provided_by = abstract_pkg_vec_alloc();
    ab_pkg->depended_upon_by = abstract_pkg_vec_alloc();
    ab_pkg->dependencies_checked = 0;
    ab_pkg->state_status = SS_NOT_INSTALLED;
}

abstract_pkg_t *abstract_pkg_new(void)
{
    abstract_pkg_t *ab_pkg;

    ab_pkg = xcalloc(1, sizeof(abstract_pkg_t));
    abstract_pkg_init(ab_pkg);

    return ab_pkg;
}

static const char *pkg_state_want_to_str(pkg_state_want_t sw)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pkg_state_want_map); i++) {
        if (pkg_state_want_map[i].value == sw) {
            return pkg_state_want_map[i].str;
        }
    }

    opkg_msg(ERROR, "Internal error: state_want=%d\n", sw);
    return "<STATE_WANT_UNKNOWN>";
}

pkg_state_want_t pkg_state_want_from_str(char *str)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pkg_state_want_map); i++) {
        if (strcmp(str, pkg_state_want_map[i].str) == 0) {
            return pkg_state_want_map[i].value;
        }
    }

    opkg_msg(ERROR, "Internal error: state_want=%s\n", str);
    return SW_UNKNOWN;
}

static char *pkg_state_flag_to_str(pkg_state_flag_t sf)
{
    unsigned int i;
    unsigned int len;
    char *str;

    /* clear the temporary flags before converting to string */
    sf &= SF_NONVOLATILE_FLAGS;

    if (sf == 0)
        return xstrdup("ok");

    len = 0;
    for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
        if (sf & pkg_state_flag_map[i].value)
            len += strlen(pkg_state_flag_map[i].str) + 1;
    }

    str = xmalloc(len + 1);
    str[0] = '\0';

    for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
        if (sf & pkg_state_flag_map[i].value) {
            strncat(str, pkg_state_flag_map[i].str, len);
            strncat(str, ",", len);
        }
    }

    len = strlen(str);
    str[len - 1] = '\0';        /* squash last comma */

    return str;
}

pkg_state_flag_t pkg_state_flag_from_str(const char *str)
{
    unsigned int i;
    int sf = SF_OK;
    const char *sfname;
    unsigned int sfname_len;

    if (strcmp(str, "ok") == 0) {
        return SF_OK;
    }
    for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
        sfname = pkg_state_flag_map[i].str;
        sfname_len = strlen(sfname);
        if (strncmp(str, sfname, sfname_len) == 0) {
            sf |= pkg_state_flag_map[i].value;
            str += sfname_len;
            if (str[0] == ',') {
                str++;
            } else {
                break;
            }
        }
    }

    return sf;
}

static const char *pkg_state_status_to_str(pkg_state_status_t ss)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pkg_state_status_map); i++) {
        if (pkg_state_status_map[i].value == ss) {
            return pkg_state_status_map[i].str;
        }
    }

    opkg_msg(ERROR, "Internal error: state_status=%d\n", ss);
    return "<STATE_STATUS_UNKNOWN>";
}

static int should_include_field(const char *field, const char *fields_filter)
{
   return field && (!fields_filter || strstr(fields_filter, field));
}

static void pkg_formatted_field(FILE * fp, pkg_t * pkg, const char *field, const char *fields_filter)
{
    unsigned int i, j;
    char *str;
    unsigned int depends_count = pkg->pre_depends_count + pkg->depends_count
        + pkg->recommends_count + pkg->suggests_count;

    if (!should_include_field(field, fields_filter)) {
       return;
    }
    if (strlen(field) < PKG_MINIMUM_FIELD_NAME_LEN) {
        goto UNKNOWN_FMT_FIELD;
    }

    switch (field[0]) {
    case 'a':
    case 'A':
        if (strcasecmp(field, "Architecture") == 0) {
            if (pkg->architecture) {
                fprintf(fp, "Architecture: %s\n", pkg->architecture);
            }
        } else if (strcasecmp(field, "Auto-Installed") == 0) {
            if (pkg->auto_installed)
                fprintf(fp, "Auto-Installed: yes\n");
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'c':
    case 'C':
        if (strcasecmp(field, "Conffiles") == 0) {
            conffile_list_elt_t *iter;

            if (nv_pair_list_empty(&pkg->conffiles))
                return;

            fprintf(fp, "Conffiles:\n");
            for (iter = nv_pair_list_first(&pkg->conffiles); iter;
                    iter = nv_pair_list_next(&pkg->conffiles, iter)) {
                conffile_t * cf = (conffile_t *) iter->data;
                if (cf->name && cf->value) {
                    fprintf(fp, " %s %s\n", ((conffile_t *) iter->data)->name,
                            ((conffile_t *) iter->data)->value);
                }
            }
        } else if (strcasecmp(field, "Conflicts") == 0) {
            struct depend *cdep;
            if (pkg->conflicts_count) {
                fprintf(fp, "Conflicts:");
                for (i = 0; i < pkg->conflicts_count; i++) {
                    cdep = pkg->conflicts[i].possibilities[0];
                    fprintf(fp, "%s %s", i == 0 ? "" : ",", cdep->pkg->name);
                    if (cdep->version) {
                        fprintf(fp, " (%s%s)",
                                constraint_to_str(cdep->constraint),
                                cdep->version);
                    }
                }
                fprintf(fp, "\n");
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'd':
    case 'D':
        if (strcasecmp(field, "Depends") == 0) {
            if (pkg->depends_count) {
                fprintf(fp, "Depends:");
                for (j = 0, i = 0; i < depends_count; i++) {
                    if (pkg->depends[i].type != DEPEND)
                        continue;
                    str = pkg_depend_str(pkg, i);
                    fprintf(fp, "%s %s", j == 0 ? "" : ",", str);
                    free(str);
                    j++;
                }
                fprintf(fp, "\n");
            }
        } else if (strcasecmp(field, "Description") == 0) {
            if (pkg->description) {
                const char* first_line_end = strchr(pkg->description, '\n');
                if (opkg_config->short_description && first_line_end) {
                    fprintf(fp, "Description: %.*s\n",
                            (int)(first_line_end - pkg->description),
                            pkg->description);
                }
                else {
                    fprintf(fp, "Description: %s\n", pkg->description);
                }
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'e':
    case 'E':
        if (strcasecmp(field, "Essential") == 0) {
            if (pkg->essential) {
                fprintf(fp, "Essential: yes\n");
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'f':
    case 'F':
        if (strcasecmp(field, "Filename") == 0) {
            if (pkg->filename) {
                fprintf(fp, "Filename: %s\n", pkg->filename);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'i':
    case 'I':
        if (strcasecmp(field, "Installed-Size") == 0) {
            if (pkg->installed_size) {
                fprintf(fp, "Installed-Size: %ld\n", pkg->installed_size);
            }
        } else if (strcasecmp(field, "Installed-Time") == 0) {
            if (pkg->installed_time) {
                fprintf(fp, "Installed-Time: %lu\n", pkg->installed_time);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'm':
    case 'M':
        if (strcasecmp(field, "Maintainer") == 0) {
            if (pkg->maintainer) {
                fprintf(fp, "Maintainer: %s\n", pkg->maintainer);
            }
        } else if (strcasecmp(field, "MD5sum") == 0) {
            if (pkg->md5sum) {
                fprintf(fp, "MD5Sum: %s\n", pkg->md5sum);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'p':
    case 'P':
        if (strcasecmp(field, "Package") == 0) {
            fprintf(fp, "Package: %s\n", pkg->name);
        } else if (strcasecmp(field, "Priority") == 0) {
            fprintf(fp, "Priority: %s\n", pkg->priority);
        } else if (strcasecmp(field, "Provides") == 0) {
            /* Don't print provides if this package provides only itself */
            if (pkg->provides_count > 1) {
                fprintf(fp, "Provides:");
                for (i = 1; i < pkg->provides_count; i++) {
                    fprintf(fp, "%s %s", i == 1 ? "" : ",",
                            pkg->provides[i]->name);
                }
                fprintf(fp, "\n");
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'r':
    case 'R':
        if (strcasecmp(field, "Replaces") == 0) {
            struct depend *rdep;
            if (pkg->replaces_count) {
                fprintf(fp, "Replaces:");
                for (i = 0; i < pkg->replaces_count; i++) {
                    rdep = pkg->replaces[i].possibilities[0];
                    fprintf(fp, "%s %s", i == 0 ? "" : ",", rdep->pkg->name);
                    if (rdep->version) {
                        fprintf(fp, " (%s%s)",
                                constraint_to_str(rdep->constraint),
                                rdep->version);
                    }
                }
                fprintf(fp, "\n");
            }
        } else if (strcasecmp(field, "Recommends") == 0) {
            if (pkg->recommends_count) {
                fprintf(fp, "Recommends:");
                for (j = 0, i = 0; i < depends_count; i++) {
                    if (pkg->depends[i].type != RECOMMEND)
                        continue;
                    str = pkg_depend_str(pkg, i);
                    fprintf(fp, "%s %s", j == 0 ? "" : ",", str);
                    free(str);
                    j++;
                }
                fprintf(fp, "\n");
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 's':
    case 'S':
        if (strcasecmp(field, "Section") == 0) {
            if (pkg->section) {
                fprintf(fp, "Section: %s\n", pkg->section);
            }
        } else if (strcasecmp(field, "SHA256sum") == 0) {
            if (pkg->sha256sum) {
                fprintf(fp, "SHA256sum: %s\n", pkg->sha256sum);
            }
        } else if (strcasecmp(field, "Size") == 0) {
            if (pkg->size) {
                fprintf(fp, "Size: %ld\n", pkg->size);
            }
        } else if (strcasecmp(field, "Source") == 0) {
            if (pkg->source) {
                fprintf(fp, "Source: %s\n", pkg->source);
            }
        } else if (strcasecmp(field, "Status") == 0) {
            char *pflag = pkg_state_flag_to_str(pkg->state_flag);
            fprintf(fp, "Status: %s %s %s\n",
                    pkg_state_want_to_str(pkg->state_want), pflag,
                    pkg_state_status_to_str(pkg->state_status));
            free(pflag);
        } else if (strcasecmp(field, "Suggests") == 0) {
            if (pkg->suggests_count) {
                fprintf(fp, "Suggests:");
                for (j = 0, i = 0; i < depends_count; i++) {
                    if (pkg->depends[i].type != SUGGEST)
                        continue;
                    str = pkg_depend_str(pkg, i);
                    fprintf(fp, "%s %s", j == 0 ? "" : ",", str);
                    free(str);
                    j++;
                }
                fprintf(fp, "\n");
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 't':
    case 'T':
        if (strcasecmp(field, "Tags") == 0) {
            if (pkg->tags) {
                fprintf(fp, "Tags: %s\n", pkg->tags);
            }
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    case 'v':
    case 'V':
        if (strcasecmp(field, "Version") == 0) {
            char *version = pkg_version_str_alloc(pkg);
            if (version == NULL)
                return;
            fprintf(fp, "Version: %s\n", version);
            free(version);
        } else {
            goto UNKNOWN_FMT_FIELD;
        }
        break;
    default:
        goto UNKNOWN_FMT_FIELD;
    }

    return;

 UNKNOWN_FMT_FIELD:
    opkg_msg(ERROR, "Internal error: field=%s\n", field);
}

static void pkg_formatted_userfields(FILE *fp, pkg_t *pkg, const char *fields_filter)
{
    nv_pair_list_elt_t *iter;

    if (nv_pair_list_empty(&pkg->userfields))
        return;

    for (iter = nv_pair_list_first(&pkg->userfields); iter;
                iter = nv_pair_list_next(&pkg->userfields, iter)) {
        nv_pair_t *uf = (nv_pair_t *)iter->data;

        if (should_include_field(uf->name, fields_filter) && uf->value) {
            fprintf(fp, "%s: %s\n", uf->name, uf->value);
        }
    }
}

pkg_state_status_t pkg_state_status_from_str(const char *str)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(pkg_state_status_map); i++) {
        if (strcmp(str, pkg_state_status_map[i].str) == 0) {
            return pkg_state_status_map[i].value;
        }
    }

    opkg_msg(ERROR, "Internal error: state_status=%s\n", str);
    return SS_NOT_INSTALLED;
}

void pkg_formatted_info(FILE * fp, pkg_t * pkg, const char *fields_filter)
{
    pkg_formatted_field(fp, pkg, "Package", NULL);
    pkg_formatted_field(fp, pkg, "Version", fields_filter);
    pkg_formatted_field(fp, pkg, "Depends", fields_filter);
    pkg_formatted_field(fp, pkg, "Recommends", fields_filter);
    pkg_formatted_field(fp, pkg, "Suggests", fields_filter);
    pkg_formatted_field(fp, pkg, "Provides", fields_filter);
    pkg_formatted_field(fp, pkg, "Replaces", fields_filter);
    pkg_formatted_field(fp, pkg, "Conflicts", fields_filter);
    pkg_formatted_field(fp, pkg, "Status", fields_filter);
    pkg_formatted_field(fp, pkg, "Section", fields_filter);
    pkg_formatted_field(fp, pkg, "Essential", fields_filter);
    pkg_formatted_field(fp, pkg, "Architecture", fields_filter);
    pkg_formatted_field(fp, pkg, "Maintainer", fields_filter);
    pkg_formatted_field(fp, pkg, "MD5sum", fields_filter);
    pkg_formatted_field(fp, pkg, "Size", fields_filter);
    pkg_formatted_field(fp, pkg, "Filename", fields_filter);
    pkg_formatted_field(fp, pkg, "Conffiles", fields_filter);
    pkg_formatted_field(fp, pkg, "Source", fields_filter);
    pkg_formatted_field(fp, pkg, "Description", fields_filter);
    pkg_formatted_field(fp, pkg, "Installed-Size", fields_filter);
    pkg_formatted_field(fp, pkg, "Installed-Time", fields_filter);
    pkg_formatted_field(fp, pkg, "Tags", fields_filter);
    if (opkg_config->verbose_status_file) {
        pkg_formatted_userfields(fp, pkg, fields_filter);
    }
    fputs("\n", fp);
}

void pkg_print_status(pkg_t * pkg, FILE * file)
{
    if (pkg == NULL) {
        return;
    }

    int is_installed = (pkg->state_status == SS_INSTALLED
            || pkg->state_status == SS_UNPACKED
            || pkg->state_status == SS_HALF_INSTALLED);

    pkg_formatted_field(file, pkg, "Package", NULL);
    pkg_formatted_field(file, pkg, "Version", NULL);
    pkg_formatted_field(file, pkg, "Depends", NULL);
    pkg_formatted_field(file, pkg, "Recommends", NULL);
    pkg_formatted_field(file, pkg, "Suggests", NULL);
    pkg_formatted_field(file, pkg, "Provides", NULL);
    pkg_formatted_field(file, pkg, "Replaces", NULL);
    pkg_formatted_field(file, pkg, "Conflicts", NULL);
    pkg_formatted_field(file, pkg, "Status", NULL);
    if (opkg_config->verbose_status_file) {
        pkg_formatted_field(file, pkg, "Section", NULL);
    }
    pkg_formatted_field(file, pkg, "Essential", NULL);
    pkg_formatted_field(file, pkg, "Architecture", NULL);
    if (opkg_config->verbose_status_file) {
        pkg_formatted_field(file, pkg, "Maintainer", NULL);
        pkg_formatted_field(file, pkg, "MD5sum", NULL);
        pkg_formatted_field(file, pkg, "Size", NULL);
        pkg_formatted_field(file, pkg, "Filename", NULL);
    }
    pkg_formatted_field(file, pkg, "Conffiles", NULL);
    if (opkg_config->verbose_status_file) {
        pkg_formatted_field(file, pkg, "Source", NULL);
        pkg_formatted_field(file, pkg, "Description", NULL);
    }
    if (is_installed) {
        pkg_formatted_field(file, pkg, "Installed-Size", NULL);
        pkg_formatted_field(file, pkg, "Installed-Time", NULL);
        pkg_formatted_field(file, pkg, "Auto-Installed", NULL);
    }
    if (opkg_config->verbose_status_file) {
        pkg_formatted_userfields(file, pkg, NULL);
    }
    fputs("\n", file);
}

/*
 * libdpkg - Debian packaging suite library routines
 * vercmp.c - comparison of version numbers
 *
 * Copyright (C) 1995 Ian Jackson <iwj10@cus.cam.ac.uk>
 */

/* assume ascii */
static int order(char x)
{
    if (x == '~')
        return -1;
    if (isdigit(x))
        return 0;
    if (!x)
        return 0;
    if (isalpha(x))
        return x;

    return 256 + (int)x;
}

static int verrevcmp(const char *val, const char *ref)
{
    if (!val)
        val = "";
    if (!ref)
        ref = "";

    while (*val || *ref) {
        int first_diff = 0;

        while ((*val && !isdigit(*val)) || (*ref && !isdigit(*ref))) {
            int vc = order(*val), rc = order(*ref);
            if (vc != rc)
                return vc - rc;
            val++;
            ref++;
        }

        while (*val == '0')
            val++;
        while (*ref == '0')
            ref++;
        while (isdigit(*val) && isdigit(*ref)) {
            if (!first_diff)
                first_diff = *val - *ref;
            val++;
            ref++;
        }
        if (isdigit(*val))
            return 1;
        if (isdigit(*ref))
            return -1;
        if (first_diff)
            return first_diff;
    }
    return 0;
}

int pkg_compare_versions_no_reinstall(const pkg_t * pkg, const pkg_t * ref_pkg)
{
    int r;

    r = pkg->epoch - ref_pkg->epoch;
    if (r)
        return r;

    r = verrevcmp(pkg->version, ref_pkg->version);
    if (r)
        return r;

    r = verrevcmp(pkg->revision, ref_pkg->revision);
    return r;
}

int pkg_compare_versions(const pkg_t * pkg, const pkg_t * ref_pkg)
{
    int r;

    r = pkg_compare_versions_no_reinstall(pkg, ref_pkg);
    if (r)
        return r;

    /* Compare force_reinstall flags. */
    r = pkg->force_reinstall - ref_pkg->force_reinstall;
    return r;
}

int pkg_version_satisfied(pkg_t * it, pkg_t * ref, const char *op)
{
    int r;

    r = pkg_compare_versions(it, ref);
    enum version_constraint constraint = str_to_constraint(&op);

    switch (constraint) {
    case EARLIER_EQUAL:
        return r <= 0;
    case LATER_EQUAL:
        return r >= 0;
    case EARLIER:
        return r < 0;
    case LATER:
        return r > 0;
    case EQUAL:
        return r == 0;
    case NONE:
        opkg_msg(ERROR, "Unknown operator: %s.\n", op);
    }
    return 0;
}

int pkg_name_version_and_architecture_compare(const void *p1, const void *p2)
{
    const pkg_t *a = *(const pkg_t **)p1;
    const pkg_t *b = *(const pkg_t **)p2;
    int namecmp;
    int vercmp;
    if (!a->name || !b->name) {
        opkg_msg(ERROR, "Internal error: a->name=%p, b->name=%p.\n", a->name,
                 b->name);
        return 0;
    }

    namecmp = strcmp(a->name, b->name);
    if (namecmp)
        return namecmp;
    vercmp = pkg_compare_versions(a, b);
    if (vercmp)
        return vercmp;
    if (!a->arch_priority || !b->arch_priority) {
        opkg_msg(ERROR,
                 "Internal error: a->arch_priority=%i b->arch_priority=%i.\n",
                 a->arch_priority, b->arch_priority);
        return 0;
    }
    if (a->arch_priority > b->arch_priority)
        return 1;
    if (a->arch_priority < b->arch_priority)
        return -1;
    return 0;
}

int abstract_pkg_name_compare(const void *p1, const void *p2)
{
    const abstract_pkg_t *a = *(const abstract_pkg_t **)p1;
    const abstract_pkg_t *b = *(const abstract_pkg_t **)p2;
    if (!a->name || !b->name) {
        opkg_msg(ERROR, "Internal error: a->name=%p b->name=%p.\n", a->name,
                 b->name);
        return 0;
    }
    return strcmp(a->name, b->name);
}

char *pkg_version_str_alloc(pkg_t * pkg)
{
    char *version;

    if (pkg->epoch) {
        if (pkg->revision)
            sprintf_alloc(&version, "%d:%s-%s", pkg->epoch, pkg->version,
                          pkg->revision);
        else
            sprintf_alloc(&version, "%d:%s", pkg->epoch, pkg->version);
    } else {
        if (pkg->revision)
            sprintf_alloc(&version, "%s-%s", pkg->version, pkg->revision);
        else
            version = xstrdup(pkg->version);
    }

    return version;
}

/*
 * XXX: this should be broken into two functions
 */
file_list_t *pkg_get_installed_files(pkg_t * pkg)
{
    int err, fd;
    char *list_file_name = NULL;
    FILE *list_file = NULL;
    char *line;
    char *installed_file_name;
    int list_from_package;

    pkg->installed_files_ref_cnt++;

    if (pkg->installed_files) {
        return pkg->installed_files;
    }

    pkg->installed_files = file_list_alloc();

    /*
     * For installed packages, look at the package.list file in the database.
     * For uninstalled packages, get the file list directly from the package.
     */
    if (pkg->state_status == SS_NOT_INSTALLED || pkg->dest == NULL)
        list_from_package = 1;
    else
        list_from_package = 0;

    if (list_from_package) {
        if (pkg->local_filename == NULL) {
            return pkg->installed_files;
        }
        /* XXX: CLEANUP: Maybe rewrite this to avoid using a temporary
         * file. In other words, change deb_extract so that it can
         * simply return the file list as a char *[] rather than
         * insisting on writing it to a FILE * as it does now. */
        sprintf_alloc(&list_file_name, "%s/%s.list.XXXXXX",
                      opkg_config->tmp_dir, pkg->name);
        fd = mkstemp(list_file_name);
        if (fd == -1) {
            opkg_perror(ERROR, "Failed to make temp file %s.", list_file_name);
            free(list_file_name);
            return pkg->installed_files;
        }
        list_file = fdopen(fd, "r+");
        if (list_file == NULL) {
            opkg_perror(ERROR, "Failed to fdopen temp file %s.",
                        list_file_name);
            close(fd);
            unlink(list_file_name);
            free(list_file_name);
            return pkg->installed_files;
        }
        err = pkg_extract_data_file_names_to_stream(pkg, list_file);
        if (err) {
            opkg_msg(ERROR, "Error extracting file list from %s.\n",
                     pkg->local_filename);
            fclose(list_file);
            unlink(list_file_name);
            free(list_file_name);
            file_list_deinit(pkg->installed_files);
            pkg->installed_files = NULL;
            return NULL;
        }
        rewind(list_file);
    } else {
        sprintf_alloc(&list_file_name, "%s/%s.list", pkg->dest->info_dir,
                      pkg->name);
        list_file = fopen(list_file_name, "r");
        if (list_file == NULL) {
            if (pkg->state_status != SS_HALF_INSTALLED)
                opkg_perror(ERROR, "Failed to open %s", list_file_name);
            free(list_file_name);
            return pkg->installed_files;
        }
        free(list_file_name);
    }

    while (1) {
        char *file_name;
        char *mode_str;
        mode_t mode = 0;
        char *link_target = NULL;
        char *readlink_buf = NULL;

        line = file_read_line_alloc(list_file);
        if (line == NULL) {
            break;
        }

        // <filename>\t<mode>\t<link_target> -- all fields except filename are optional
        file_name = line;
        mode_str = strchr(line, '\t');
        if (mode_str) {
            *mode_str++ = 0;
            link_target = strchr(mode_str, '\t');
            if (link_target)
                *link_target++ = 0;
            mode = (mode_t)strtoul(mode_str, NULL, 0);
        }

        if (list_from_package) {
            if (*file_name == '.') {
                file_name++;
            }
            if (*file_name == '/') {
                file_name++;
            }
            sprintf_alloc(&installed_file_name, "%s%s", pkg->dest->root_dir,
                          file_name);
        } else {
            struct stat file_stat;
            int unmatched_offline_root = opkg_config->offline_root
                    && !str_starts_with(file_name, opkg_config->offline_root);
            if (unmatched_offline_root) {
                sprintf_alloc(&installed_file_name, "%s%s",
                              opkg_config->offline_root, file_name);
            } else {
                // already contains root_dir as header -> ABSOLUTE
                sprintf_alloc(&installed_file_name, "%s", file_name);
            }
            if (!mode && xlstat(installed_file_name, &file_stat) == 0)
                mode = file_stat.st_mode;
            if (!link_target && S_ISLNK(mode))
                link_target = readlink_buf = file_readlink_alloc(installed_file_name);
        }
        file_list_append(pkg->installed_files, installed_file_name, mode, link_target);
        free(installed_file_name);
        free(readlink_buf);
        free(line);
    }

    fclose(list_file);

    if (list_from_package) {
        unlink(list_file_name);
        free(list_file_name);
    }

    return pkg->installed_files;
}

/* XXX: CLEANUP: This function and it's counterpart,
   (pkg_get_installed_files), do not match our init/deinit naming
   convention. Nor the alloc/free convention. But, then again, neither
   of these conventions currrently fit the way these two functions
   work. */
void pkg_free_installed_files(pkg_t * pkg)
{
    pkg->installed_files_ref_cnt--;

    if (pkg->installed_files_ref_cnt > 0)
        return;

    if (pkg->installed_files) {
        file_list_purge(pkg->installed_files);
    }

    pkg->installed_files = NULL;
}

void pkg_remove_installed_files_list(pkg_t * pkg)
{
    char *list_file_name;

    sprintf_alloc(&list_file_name, "%s/%s.list", pkg->dest->info_dir,
                  pkg->name);

    if (!opkg_config->noaction)
        (void)unlink(list_file_name);

    free(list_file_name);
}

conffile_t *pkg_get_conffile(pkg_t * pkg, const char *file_name)
{
    conffile_list_elt_t *iter;
    conffile_t *conffile;

    if (pkg == NULL) {
        return NULL;
    }

    for (iter = nv_pair_list_first(&pkg->conffiles); iter;
            iter = nv_pair_list_next(&pkg->conffiles, iter)) {
        conffile = (conffile_t *) iter->data;

        if (strcmp(conffile->name, file_name) == 0) {
            return conffile;
        }
    }

    return NULL;
}

int pkg_run_script(pkg_t * pkg, const char *script, const char *args)
{
    int err;
    char *path;
    char *cmd;

    if (opkg_config->noaction)
        return 0;

    if (opkg_config->offline_root && !opkg_config->force_postinstall) {
        opkg_msg(INFO, "Offline root mode: not running %s.%s.\n", pkg->name,
                 script);
        return 0;
    }

    /* Installed packages have scripts in pkg->dest->info_dir, uninstalled packages
     * have scripts in pkg->tmp_unpack_dir. */
    if (pkg->state_status == SS_INSTALLED || pkg->state_status == SS_UNPACKED ||
        pkg->state_status == SS_HALF_INSTALLED) {
        if (pkg->dest == NULL) {
            opkg_msg(ERROR, "Internal error: %s has a NULL dest.\n", pkg->name);
            return -1;
        }
        sprintf_alloc(&path, "%s/%s.%s", pkg->dest->info_dir, pkg->name,
                      script);
    } else {
        if (pkg->tmp_unpack_dir == NULL) {
            opkg_msg(ERROR, "Internal error: %s has a NULL tmp_unpack_dir.\n",
                     pkg->name);
            return -1;
        }
        sprintf_alloc(&path, "%s/%s", pkg->tmp_unpack_dir, script);
    }

    opkg_msg(INFO, "Running script %s.\n", path);

    setenv("PKG_ROOT",
           pkg->dest ? pkg->dest->root_dir : opkg_config->default_dest->root_dir,
           1);

    if (!file_exists(path)) {
        free(path);
        return 0;
    }

    sprintf_alloc(&cmd, "%s %s", path, args);
    free(path);
    {
        const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
        err = xsystem(argv);
    }
    free(cmd);

    if (err) {
        if (!opkg_config->offline_root)
            opkg_msg(ERROR, "package \"%s\" %s script returned status %d.\n",
                     pkg->name, script, err);
        return err;
    }

    return 0;
}

int pkg_arch_supported(pkg_t * pkg)
{
    nv_pair_list_elt_t *l;

    if (!pkg->architecture)
        return 1;

    list_for_each_entry(l, &opkg_config->arch_list.head, node) {
        nv_pair_t *nv = (nv_pair_t *) l->data;
        if (strcmp(nv->name, pkg->architecture) == 0) {
            opkg_msg(DEBUG, "Arch %s (priority %s) supported for pkg %s.\n",
                     nv->name, nv->value, pkg->name);
            return 1;
        }
    }

    opkg_msg(DEBUG, "Arch %s unsupported for pkg %s.\n", pkg->architecture,
             pkg->name);
    return 0;
}

void pkg_info_preinstall_check(void)
{
    unsigned int i;
    pkg_vec_t *installed_pkgs = pkg_vec_alloc();

    /* update the file owner data structure */
    opkg_msg(INFO, "Updating file owner list.\n");
    pkg_hash_fetch_all_installed(installed_pkgs, INSTALLED);
    for (i = 0; i < installed_pkgs->len; i++) {
        pkg_t *pkg = installed_pkgs->pkgs[i];
        file_list_t *installed_files = pkg_get_installed_files(pkg);    /* this causes installed_files to be cached */
        file_list_elt_t *iter, *niter;
        if (installed_files == NULL) {
            opkg_msg(ERROR,
                     "Failed to determine installed " "files for pkg %s.\n",
                     pkg->name);
            break;
        }
        for (iter = file_list_first(installed_files), niter = file_list_next(installed_files, iter);
                iter;
                iter = niter, niter = file_list_next(installed_files, iter)) {
            file_info_t *installed_file = (file_info_t *)iter->data;
            file_hash_set_file_owner(installed_file->path, pkg);
        }
        pkg_free_installed_files(pkg);
    }
    pkg_vec_free(installed_pkgs);
}

struct pkg_write_filelist_data {
    pkg_t *pkg;
    FILE *stream;
};

static void pkg_write_filelist_helper(const char *key, void *entry_,
                                      void *data_)
{
    struct pkg_write_filelist_data *data = data_;
    pkg_t *entry = entry_;
    if (entry == data->pkg) {
        char *installed_file_name;
        struct stat file_stat;
        mode_t mode = 0;
        char *link_target = NULL;
        size_t size;
        int unmatched_offline_root = opkg_config->offline_root
                && !str_starts_with(key, opkg_config->offline_root);
        char *entry = xstrdup(key);

        size = strlen(entry);
        if (size > 0 && entry[size-1] == '/')
            entry[size-1] = '\0';

        if (unmatched_offline_root) {
            sprintf_alloc(&installed_file_name, "%s%s",
                          opkg_config->offline_root, entry);
        } else {
            // already contains root_dir as header -> ABSOLUTE
            sprintf_alloc(&installed_file_name, "%s", entry);
        }

        if (xlstat(installed_file_name, &file_stat) == 0) {
            mode = file_stat.st_mode;
            if (S_ISLNK(mode))
                link_target = file_readlink_alloc(installed_file_name);
        }

        if (link_target)
            fprintf(data->stream, "%s\t%#03o\t%s\n", entry, (unsigned int)mode, link_target);
        else if (mode)
            fprintf(data->stream, "%s\t%#03o\n", entry, (unsigned int)mode);
        else
            fprintf(data->stream, "%s\n", entry);

        free(entry);
        free(link_target);
        free(installed_file_name);
    }
}

int pkg_write_filelist(pkg_t * pkg)
{
    struct pkg_write_filelist_data data;
    char *list_file_name;

    sprintf_alloc(&list_file_name, "%s/%s.list", pkg->dest->info_dir,
                  pkg->name);

    opkg_msg(INFO, "Creating %s file for pkg %s.\n", list_file_name, pkg->name);

    data.stream = fopen(list_file_name, "w");
    if (!data.stream) {
        opkg_perror(ERROR, "Failed to open %s", list_file_name);
        free(list_file_name);
        return -1;
    }

    data.pkg = pkg;
    hash_table_foreach(&opkg_config->file_hash, pkg_write_filelist_helper,
                       &data);
    fclose(data.stream);
    free(list_file_name);

    pkg->state_flag &= ~SF_FILELIST_CHANGED;

    return 0;
}

int pkg_write_changed_filelists(void)
{
    pkg_vec_t *installed_pkgs = pkg_vec_alloc();
    unsigned int i;
    int err, ret = 0;

    if (opkg_config->noaction)
        return 0;

    opkg_msg(INFO, "Saving changed filelists.\n");

    pkg_hash_fetch_all_installed(installed_pkgs, INSTALLED);
    for (i = 0; i < installed_pkgs->len; i++) {
        pkg_t *pkg = installed_pkgs->pkgs[i];
        if (pkg->state_flag & SF_FILELIST_CHANGED) {
            err = pkg_write_filelist(pkg);
            if (err)
                ret = -1;
        }
    }

    pkg_vec_free(installed_pkgs);

    return ret;
}

int pkg_verify(pkg_t * pkg)
{
    int err;
    struct stat pkg_stat;
    char *local_sig_filename = NULL;

    err = stat(pkg->local_filename, &pkg_stat);
    if (err) {
        if (errno == ENOENT) {
            /* Exit with soft error 1 if the package doesn't exist.
             * This allows the caller to download it without nasty
             * messages in the error log.
             */
            return 1;
        }
        else {
            opkg_msg(ERROR, "Failed to stat %s: %s\n",
                pkg->local_filename, strerror(errno));
            goto fail;
        }
    }

    /* Check size to mitigate hash collisions. */
    if (pkg_stat.st_size < 1 || pkg_stat.st_size != pkg->size) {
        err = -1;
        opkg_msg(ERROR, "File size mismatch: %s is %lld bytes, expecting %lu bytes\n",
            pkg->local_filename, (long long int)pkg_stat.st_size, pkg->size);
        goto fail;
    }

#ifdef HAVE_SHA256
    if (pkg->sha256sum) {
        err = opkg_verify_sha256sum(pkg->local_filename, pkg->sha256sum);
        if (err)
            goto fail;
    }
    else
#endif
    if (pkg->md5sum) {
        err = opkg_verify_md5sum(pkg->local_filename, pkg->md5sum);
        if (err)
            goto fail;
    } else if (!opkg_config->force_checksum) {
         opkg_msg(ERROR, "Checksum is either missing or unsupported on opkg. To bypass verification "
                  "use --force-checksum. Aborting \n");
         return -1;
    }

    if (opkg_config->check_pkg_signature) {
        local_sig_filename = pkg_download_signature(pkg);
        if (!local_sig_filename) {
            err = -1;
            goto fail;
        }

        err = opkg_verify_signature(pkg->local_filename, local_sig_filename);
        if (err)
            goto fail;

	opkg_msg(DEBUG, "Signature verification passed for %s.\n", pkg->local_filename);
    }

    free(local_sig_filename);
    return 0;

 fail:
    if (!opkg_config->force_checksum)
    {
        opkg_msg(NOTICE, "Removing corrupt package file %s.\n",
             pkg->local_filename);
        unlink(pkg->local_filename);
        if (local_sig_filename && file_exists(local_sig_filename))
        {
            opkg_msg(NOTICE, "Removing unmatched signature file %s.\n",
                 local_sig_filename);
            unlink(local_sig_filename);
        }
    }
    else
    {
	opkg_msg(NOTICE, "Ignored %s checksum mismatch.\n",
             pkg->local_filename);
        err = 0;
    }
    free(local_sig_filename);
    return err;
}
