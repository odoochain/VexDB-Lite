/*
 * Copyright (c) 2026 VexDB-THU
 * Definitions for distance names
 */
#ifndef DOT_KERNEL_ASIMD_H
#define DOT_KERNEL_ASIMD_H

#include <arm_neon.h>
#define DOT_MOD	"s"
#define REG0		"wzr"
#define TMPX		"s16"
#define TMPY		"s24"
#define N_DIV_SHIFT	"6"
#define N_REM_MASK	"63"

#define OUT		"%" DOT_MOD "[DOT_]"

#define KERNEL_F1                                               \
        "       ldr     " TMPX ", [%[X_]]                       \n"     \
        "       ldr     " TMPY ", [%[Y_]]                       \n"     \
        "       add     %[X_], %[X_], #4                        \n"     \
        "       add     %[Y_], %[Y_], #4                        \n"     \
        "       fmadd   " OUT ", " TMPX ", " TMPY ", " OUT "    \n"

#define KERNEL_F						\
	"	ldp	q16, q17, [%[X_]]		\n"	\
	"	ldp	q24, q25, [%[Y_]]		\n"	\
	"	ldp	q18, q19, [%[X_], #32]		\n"	\
	"	ldp	q26, q27, [%[Y_], #32]		\n"	\
	"	fmla	v0.4s, v16.4s, v24.4s		\n"	\
	"	fmla	v1.4s, v17.4s, v25.4s		\n"	\
	"	ldp	q20, q21, [%[X_], #64]		\n"	\
	"	ldp	q28, q29, [%[Y_], #64]		\n"	\
	"	fmla	v2.4s, v18.4s, v26.4s		\n"	\
	"	fmla	v3.4s, v19.4s, v27.4s		\n"	\
	"	ldp	q22, q23, [%[X_], #96]		\n"	\
	"	ldp	q30, q31, [%[Y_], #96]		\n"	\
	"	add	%[Y_], %[Y_], #128		\n"	\
	"	add	%[X_], %[X_], #128		\n"	\
	"	fmla	v4.4s, v20.4s, v28.4s		\n"	\
	"	fmla	v5.4s, v21.4s, v29.4s		\n"	\
	"	PRFM	PLDL1KEEP, [%[X_], #896]	\n"	\
	"	PRFM	PLDL1KEEP, [%[Y_], #896]	\n"	\
	"	PRFM	PLDL1KEEP, [%[X_], #896+64]	\n"	\
	"	PRFM	PLDL1KEEP, [%[Y_], #896+64]	\n"	\
	"	fmla	v6.4s, v22.4s, v30.4s		\n"	\
	"	fmla	v7.4s, v23.4s, v31.4s		\n"	\
	"	ldp	q16, q17, [%[X_]]		\n"	\
	"	ldp	q24, q25, [%[Y_]]		\n"	\
	"	ldp	q18, q19, [%[X_], #32]		\n"	\
	"	ldp	q26, q27, [%[Y_], #32]		\n"	\
	"	fmla	v0.4s, v16.4s, v24.4s		\n"	\
	"	fmla	v1.4s, v17.4s, v25.4s		\n"	\
	"	ldp	q20, q21, [%[X_], #64]		\n"	\
	"	ldp	q28, q29, [%[Y_], #64]		\n"	\
	"	fmla	v2.4s, v18.4s, v26.4s		\n"	\
	"	fmla	v3.4s, v19.4s, v27.4s		\n"	\
	"	ldp	q22, q23, [%[X_], #96]		\n"	\
	"	ldp	q30, q31, [%[Y_], #96]		\n"	\
	"	add	%[Y_], %[Y_], #128		\n"	\
	"	add	%[X_], %[X_], #128		\n"	\
	"	fmla	v4.4s, v20.4s, v28.4s		\n"	\
	"	fmla	v5.4s, v21.4s, v29.4s		\n"	\
	"	PRFM	PLDL1KEEP, [%[X_], #896]	\n"	\
	"	PRFM	PLDL1KEEP, [%[Y_], #896]	\n"	\
	"	PRFM	PLDL1KEEP, [%[X_], #896+64]	\n"	\
	"	PRFM	PLDL1KEEP, [%[Y_], #896+64]	\n"	\
	"	fmla	v6.4s, v22.4s, v30.4s		\n"	\
	"	fmla	v7.4s, v23.4s, v31.4s		\n"


#define KERNEL_F_FINALIZE					\
	"	fadd	v0.4s, v0.4s, v1.4s		\n"	\
	"	fadd	v2.4s, v2.4s, v3.4s		\n"	\
	"	fadd	v4.4s, v4.4s, v5.4s		\n"	\
	"	fadd	v6.4s, v6.4s, v7.4s		\n"	\
	"	fadd	v0.4s, v0.4s, v2.4s		\n"	\
	"	fadd	v4.4s, v4.4s, v6.4s		\n"	\
	"	fadd	v0.4s, v0.4s, v4.4s		\n"	\
	"	faddp	v0.4s, v0.4s, v0.4s		\n"	\
	"	faddp	" OUT ", v0.2s			\n"

static FORCE_INLINE float dot_kernel_asimd(size_t dim, const float *const x, const float *const y)
{
	float dot = 0.0;
	size_t j = 0;
	const float *x_tmp;
	const float *y_tmp;

	__asm__ __volatile__ (
	"	mov	%[X_], %[X_in]			\n"
	"	mov	%[Y_], %[Y_in]			\n"
	"	movi	v0.4s, #0				\n"
	"	movi	v1.4s, #0				\n"
	"	movi	v2.4s, #0				\n"
	"	movi	v3.4s, #0				\n"
	"	movi	v4.4s, #0				\n"
	"	movi	v5.4s, #0				\n"
	"	movi	v6.4s, #0				\n"
	"	movi	v7.4s, #0				\n"
	"	fmov	" OUT ", " REG0 "			\n"

	"1: //dot_kernel_F_BEGIN:			\n"
	"	asr	%[J_], %[N_], #" N_DIV_SHIFT "	\n"
	"	cmp	%[J_], xzr			\n"
	"	beq	3f //dot_kernel_F1		\n"

        "2: //dot_kernel_F:				\n"
	"	" KERNEL_F "				\n"
	"	subs	%[J_], %[J_], #1		\n"
	"	bne	2b //dot_kernel_F		\n"
	"	" KERNEL_F_FINALIZE "			\n"

	"3: //dot_kernel_F1:				\n"
	"	ands	%[J_], %[N_], #" N_REM_MASK "	\n"
	"	ble	9f //dot_kernel_L999		\n"

	"4: //dot_kernel_F10:				\n"
	"	" KERNEL_F1 "				\n"
	"	subs	%[J_], %[J_], #1		\n"
	"	bne	4b //dot_kernel_F10		\n"
	"	b	9f //dot_kernel_L999		\n"

	"9: //dot_kernel_L999:				\n"

	: [DOT_]  "=&w" (dot),
	  [X_]    "=&r" (x_tmp),
	  [Y_]    "=&r" (y_tmp),
	  [J_]    "+r"  (j)
	: [N_]    "r"   (dim),
	  [X_in]  "r"   (x),
	  [Y_in]  "r"   (y)

	: "cc",
	  "memory",
	  "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
	  "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
	  "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
	);
	return dot;
}

#endif
