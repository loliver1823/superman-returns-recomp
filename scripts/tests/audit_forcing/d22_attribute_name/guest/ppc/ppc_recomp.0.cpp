#include "ppc_recomp_shared.h"

__attribute__((alias("__imp__sub_82003000"))) PPC_WEAK_FUNC(sub_82003000);  // EXPECT-GEN: NOSITE
PPC_FUNC_IMPL(__imp__sub_82003000) {
	PPC_FUNC_PROLOGUE();
	// blr
	return;
}

__attribute__((alias("__imp__sub_82003010"))) PPC_WEAK_FUNC(sub_82003010);  // EXPECT-GEN: NOSITE
PPC_FUNC_IMPL(__imp__sub_82003010) {
	PPC_FUNC_PROLOGUE();
	// blr
	return;
}
