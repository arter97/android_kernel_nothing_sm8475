#ifndef _ASM_ARM64_VMALLOC_H
#define _ASM_ARM64_VMALLOC_H

#include <asm/page.h>
#include <asm/pgtable.h>

#define arch_vmap_pgprot_tagged arch_vmap_pgprot_tagged
static inline pgprot_t arch_vmap_pgprot_tagged(pgprot_t prot)
{
	return pgprot_tagged(prot);
}

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
bool arch_vmap_p4d_supported(pgprot_t prot);
bool arch_vmap_pud_supported(pgprot_t prot);
bool arch_vmap_pmd_supported(pgprot_t prot);
#endif

#endif /* _ASM_ARM64_VMALLOC_H */
