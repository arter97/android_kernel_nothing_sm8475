#include "selinux.h"
#include "linux/cred.h"
#include "linux/sched.h"
#include "objsec.h"
#include "linux/version.h"
#include "../klog.h" // IWYU pragma: keep

#define KERNEL_SU_DOMAIN "u:r:su:s0"

static int transive_to_domain(const char *domain)
{
    struct cred *cred;
    struct task_security_struct *tsec;
    u32 sid;
    int error;

    cred = (struct cred *)__task_cred(current);

    tsec = cred->security;
    if (!tsec) {
        pr_err("tsec == NULL!\n");
        return -1;
    }

    error = security_secctx_to_secid(domain, strlen(domain), &sid);
    if (error) {
        pr_info("security_secctx_to_secid %s -> sid: %d, error: %d\n",
            domain, sid, error);
    }
    if (!error) {
        tsec->sid = sid;
        tsec->create_sid = 0;
        tsec->keycreate_sid = 0;
        tsec->sockcreate_sid = 0;
    }
    return error;
}

void setup_selinux(const char *domain)
{
    if (transive_to_domain(domain)) {
        pr_err("transive domain failed.\n");
        return;
    }
}

void setenforce(bool enforce)
{
#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
    selinux_state.enforcing = enforce;
#endif
}

bool getenforce()
{
#ifdef CONFIG_SECURITY_SELINUX_DISABLE
    if (selinux_state.disabled) {
        return false;
    }
#endif

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
    return selinux_state.enforcing;
#else
    return true;
#endif
}

extern int security_sid_to_context_stack(u32 sid, char **scontext, u32 *scontext_len);

bool is_task_ksu_domain(const struct cred* cred)
{
    struct lsm_context ctx;
    bool result;
    if (!cred) {
        return false;
    }
    const struct task_security_struct *tsec = selinux_cred(cred);
    if (!tsec) {
        return false;
    }
    char domain_buf[SELINUX_LABEL_LENGTH];
    char *domain;
    u32 seclen;
    int err;
    bool match;

    domain = domain_buf;
    err = security_sid_to_context_stack(tsec->sid, &domain, &seclen);
    if (err)
        return false;

    match = !strncmp(KERNEL_SU_DOMAIN, domain, seclen);

    return match;
}

bool is_ksu_domain()
{
    current_sid();
    return is_task_ksu_domain(current_cred());
}

bool is_context(const struct cred* cred, const char* context)
{
    if (!cred) {
        return false;
    }
    const struct task_security_struct * tsec = selinux_cred(cred);
    if (!tsec) {
        return false;
    }
    struct lsm_context ctx;
    char domain_buf[SELINUX_LABEL_LENGTH];
    char *domain;
    u32 seclen;
    int err;
    bool match;

    domain = domain_buf;
    err = security_sid_to_context_stack(tsec->sid, &domain, &seclen);
    if (err)
        return false;

    match = !strncmp(context, domain, seclen);

    return match;
}

bool is_zygote(const struct cred* cred)
{
    return is_context(cred, "u:r:zygote:s0");
}

bool is_init(const struct cred* cred) {
    return is_context(cred, "u:r:init:s0");
}

#define KSU_FILE_DOMAIN "u:object_r:ksu_file:s0"

u32 ksu_get_ksu_file_sid()
{
    u32 ksu_file_sid = 0;
    int err = security_secctx_to_secid(KSU_FILE_DOMAIN, strlen(KSU_FILE_DOMAIN),
                       &ksu_file_sid);
    if (err) {
        pr_info("get ksufile sid err %d\n", err);
    }
    return ksu_file_sid;
}
