#include "distance/include/architecture_macro.h"

#if COMPILER_SUPPORT_SMEV9
#include "distance/include/distance.h"
#include "distance/include/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  SMEV9_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) SMEV9_STRUCT(name)
#define __NEON_SUPPORT__
#define __SVE_SUPPORT__
#define __SVE2_SUPPORT__
#define __SME_SUPPORT__
#include "../../distances_simd_template.cpp"
#include "../../code_distance_template.cpp"
#include "../../rabitq_template.cpp"
#include "../../template_half.cpp"
#endif /* COMPILER_SUPPORT_SMEV9 */
