#include "distance/include/architecture_macro.h"

#if COMPILER_SUPPORT_AVX
#include "distance/include/distance_utils.h"
#include "distance/include/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  AVX_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) AVX_STRUCT(name)
#define __SSE_SUPPORT__
#define __AVX_SUPPORT__
#include "../../distances_simd_template.cpp"
#include "../../code_distance_template.cpp"
#include "../../rabitq_template.cpp"
#include "../../template_half.cpp"
#endif /* COMPILER_SUPPORT_AVX */
