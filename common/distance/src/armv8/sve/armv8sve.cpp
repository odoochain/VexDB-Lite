#include "distance/include/architecture_macro.h"

#if COMPILER_SUPPORT_SVEV8
#include "distance/include/distance_utils.h"
#include "distance/include/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  SVEV8_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) SVEV8_STRUCT(name)
#define __NEON_SUPPORT__
#define __SVE_SUPPORT__
#include "../../distances_simd_template.cpp"
#include "../../code_distance_template.cpp"
#include "../../rabitq_template.cpp"
#include "../../template_half.cpp"
#endif /* COMPILER_SUPPORT_SVEV8 */
