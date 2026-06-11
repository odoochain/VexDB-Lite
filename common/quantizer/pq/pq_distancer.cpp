#include <math.h>
#include "quantizer/pq/pq_distancer.h"


void PQDistancer::train(Relation index, FloatVectorArray samples, size_t dimension, Metric metric,
    bool need_norm, int parallel_workers, int maintenance_work_mem)
{
    uint16 m;
    uint16 k;
    pq_set_param(dimension, m, k);
    pq.set_basic_values(dimension, m, std::log2(k));
    pq.set_fvec_L2sqr_ny_nearest_func();
    pq.train(samples, metric, parallel_workers, maintenance_work_mem);
}

void PQDistancer::destroy()
{
    if (!prepared) {
        return;
    }
    free_vector(dist_table);
    pq.free_resourses();
    prepared = false;
}
