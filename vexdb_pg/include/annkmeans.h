#ifndef VEXDB_PG_ANNKMEANS_H
#define VEXDB_PG_ANNKMEANS_H

#include "quantizer/annkmeans.h"
#include "access/reloptions.h"

#define MAX_SAMPLE_VECTOR_NUM 50000

extern int GetSampleNumbers(Relation heap, Relation index, int listNum);
extern void ann_sample_rows(FloatVectorArray samples, Relation heap,
                            Relation index, int dimensions, int sample_nums,
                            bool need_norm,
                            DistPrecisionType dist_type = DistPrecisionType::FLOAT);

#endif
