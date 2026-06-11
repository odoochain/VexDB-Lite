/**
 * Copyright ...
 * RaBitQ Random Orthogonal Matrix
 */

#include <cstring>

#include "quantizer/rabitq/utils.h"
#include "quantizer/rabitq/rotator.h"

namespace rabitq {

void FhtKacRotator::build()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto &i : _flip) {
        i = static_cast<uint8>(dist(gen));
    }
}

void FhtKacRotator::rotate(float *data, float *rotated)
{
    std::memcpy(rotated, data, sizeof(float) * _dim);
    std::fill(rotated + _dim, rotated + _padded_dim, 0);

    if (_trunc_dim == _padded_dim) {
        _flip_sign(_flip.data(), rotated, _padded_dim);
        _fht_float(rotated);
        vec_rescale(rotated, _trunc_dim, _fac);

        _flip_sign(_flip.data() + (_padded_dim / kByteLen), rotated, _padded_dim);
        _fht_float(rotated);
        vec_rescale(rotated, _trunc_dim, _fac);

        _flip_sign(
            _flip.data() + (2 * _padded_dim / kByteLen), rotated, _padded_dim
        );
        _fht_float(rotated);
        vec_rescale(rotated, _trunc_dim, _fac);

        _flip_sign(
            _flip.data() + (3 * _padded_dim / kByteLen), rotated, _padded_dim
        );
        _fht_float(rotated);
        vec_rescale(rotated, _trunc_dim, _fac);

        return;
    }

    size_t start = _padded_dim - _trunc_dim;

    _flip_sign(_flip.data(), rotated, _padded_dim);
    _fht_float(rotated);
    vec_rescale(rotated, _trunc_dim, _fac);
    _kacs_walk(rotated, _padded_dim);

    _flip_sign(_flip.data() + (_padded_dim / kByteLen), rotated, _padded_dim);
    _fht_float(rotated + start);
    vec_rescale(rotated + start, _trunc_dim, _fac);
    _kacs_walk(rotated, _padded_dim);

    _flip_sign(_flip.data() + (2 * _padded_dim / kByteLen), rotated, _padded_dim);
    _fht_float(rotated);
    vec_rescale(rotated, _trunc_dim, _fac);
    _kacs_walk(rotated, _padded_dim);

    _flip_sign(_flip.data() + (3 * _padded_dim / kByteLen), rotated, _padded_dim);
    _fht_float(rotated + start);
    vec_rescale(rotated + start, _trunc_dim, _fac);
    _kacs_walk(rotated, _padded_dim);

    /* This can be removed if we don't care about the absolute value of similarities */
    vec_rescale(rotated, _padded_dim, 0.25f);
}

} /* namespace rabitq */
