#include "usdt.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t
_dof_version(uint8_t header_version)
{
        uint8_t dof_version;
        /* DOF versioning: Apple always needs version 3, but Solaris can use
           1 or 2 depending on whether is-enabled probes are needed. */
#ifdef __APPLE__
        dof_version = DOF_VERSION_3;
#else
        switch(header_version) {
        case 1:
                dof_version = DOF_VERSION_1;
                break;
        case 2:
                dof_version = DOF_VERSION_2;
                break;
        default:
                dof_version = DOF_VERSION;
        }
#endif
        return dof_version;
}

#ifdef __APPLE__
static const char *helper = "/dev/dtracehelper";

static int
_loaddof(int fd, dof_helper_t *dh)
{
        int ret;
        uint8_t buffer[sizeof(dof_ioctl_data_t) + sizeof(dof_helper_t)];
        dof_ioctl_data_t* ioctlData = (dof_ioctl_data_t*)buffer;
        user_addr_t val;

        ioctlData->dofiod_count = 1;
        memcpy(&ioctlData->dofiod_helpers[0], dh, sizeof(dof_helper_t));

        val = (user_addr_t)(unsigned long)ioctlData;
        ret = ioctl(fd, DTRACEHIOC_ADDDOF, &val);

        return ret;
}

#else /* Solaris */

/* ignore Sol10 GA ... */
static const char *helper = "/dev/dtrace/helper";

static int _loaddof(int fd, dof_helper_t *dh) {
        return ioctl(fd, DTRACEHIOC_ADDDOF, dh);
}

#endif

void
usdt_dof_file_append_section(usdt_dof_file_t *file, usdt_dof_section_t *section)
{
        usdt_dof_section_t *s;

        if (file->sections == NULL) {
                file->sections = section;
        }
        else {
                for (s = file->sections; (s->next != NULL); s = s->next) ;
                s->next = section;
        }
}

void
usdt_dof_file_generate(usdt_dof_file_t *file, usdt_strtab_t *strtab)
{
        dof_hdr_t header;
        uint64_t filesz;
        uint64_t loadsz;
        usdt_dof_section_t *sec;
        size_t pad = 0;
        size_t i = 0;
        size_t offset;
        char *h;

        memset(&header, 0, sizeof(header));

        header.dofh_ident[DOF_ID_MAG0] = DOF_MAG_MAG0;
        header.dofh_ident[DOF_ID_MAG1] = DOF_MAG_MAG1;
        header.dofh_ident[DOF_ID_MAG2] = DOF_MAG_MAG2;
        header.dofh_ident[DOF_ID_MAG3] = DOF_MAG_MAG3;

        header.dofh_ident[DOF_ID_MODEL]    = DOF_MODEL_NATIVE;
        header.dofh_ident[DOF_ID_ENCODING] = DOF_ENCODE_NATIVE;
        header.dofh_ident[DOF_ID_DIFVERS]  = DIF_VERSION;
        header.dofh_ident[DOF_ID_DIFIREG]  = DIF_DIR_NREGS;
        header.dofh_ident[DOF_ID_DIFTREG]  = DIF_DTR_NREGS;

        /* default 2, will be 3 on OSX */
        header.dofh_ident[DOF_ID_VERSION] = _dof_version(2);
        header.dofh_hdrsize = sizeof(dof_hdr_t);
        header.dofh_secsize = sizeof(dof_sec_t);
        header.dofh_secoff = sizeof(dof_hdr_t);

        header.dofh_secnum = 6;

        filesz = sizeof(dof_hdr_t) + (sizeof(dof_sec_t) * header.dofh_secnum);
        loadsz = filesz;

        strtab->offset = filesz;

        if (strtab->align > 1) {
                i = strtab->offset % strtab->align;
                if (i > 0) {
                        pad = strtab->align - i;
                        strtab->offset = (pad + strtab->offset);
                        strtab->pad = pad;
                }
        }

        filesz += strtab->size + pad;

        if (strtab->flags & 1)
                loadsz += strtab->size + pad;

        for (sec = file->sections; sec != NULL; sec = sec->next) {
                pad = 0;
                i = 0;

                sec->offset = filesz;

                if (sec->align > 1) {
                        i = sec->offset % sec->align;
                        if (i > 0) {
                                pad = sec->align - i;
                                sec->offset = (pad + sec->offset);
                                sec->pad = pad;
                        }
                }

                filesz += sec->size + pad;
                if (sec->flags & 1)
                        loadsz += sec->size + pad;
        }

        header.dofh_loadsz = loadsz;
        header.dofh_filesz = filesz;
        memcpy(file->dof, &header, sizeof(dof_hdr_t));

        offset = sizeof(dof_hdr_t);

        h = usdt_strtab_header(strtab);
        memcpy((file->dof + offset), h, sizeof(dof_sec_t));
        free(h);
        offset += sizeof(dof_sec_t);

        for (sec = file->sections; sec != NULL; sec = sec->next) {
                h = usdt_dof_section_header(sec);
                memcpy((file->dof + offset), h, sizeof(dof_sec_t));
                free(h);
                offset += sizeof(dof_sec_t);
        }

        if (strtab->pad > 0) {
                memcpy((file->dof + offset), "\0", strtab->pad);
                offset += strtab->pad;
        }
        memcpy((file->dof + offset), strtab->data, strtab->size);
        offset += strtab->size;

        for (sec = file->sections; sec != NULL; sec = sec->next) {
                if (sec->pad > 0) {
                        memcpy((file->dof + offset), "\0", sec->pad);
                        offset += sec->pad;
                }
                memcpy((file->dof + offset), sec->data, sec->size);
                offset += sec->size;
        }
}

void
usdt_dof_file_load(usdt_dof_file_t *file)
{
        dof_helper_t dh;
        dof_hdr_t *dof;
        int fd;

        dof = (dof_hdr_t *) file->dof;

        dh.dofhp_dof  = (uintptr_t)dof;
        dh.dofhp_addr = (uintptr_t)dof;
        (void) snprintf(dh.dofhp_mod, sizeof (dh.dofhp_mod), "module");

        fd = open(helper, O_RDWR);
        file->gen = _loaddof(fd, &dh);

        (void) close(fd);
}

usdt_dof_file_t *
usdt_dof_file_init(usdt_provider_t *provider, size_t size)
{
        usdt_dof_file_t *file;

        if ((file = malloc(sizeof(*file))) == NULL)
                return (NULL);

        file->sections = NULL;
        file->size = size;
        file->dof = (char *) valloc(file->size);

        return (file);
}
