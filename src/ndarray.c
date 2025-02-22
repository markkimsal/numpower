#include <stdio.h>
#include "ndarray.h"
#include "debug.h"
#include "iterators.h"
#include "ndmath/arithmetics.h"
#include "initializers.h"
#include "types.h"
#include "logic.h"
#include <php.h>
#include "../config.h"
#include "Zend/zend_alloc.h"
#include "Zend/zend_API.h"
#include <Zend/zend_types.h>

#ifdef HAVE_AVX2
#include <immintrin.h>
#endif

#ifdef HAVE_CUBLAS
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include "ndmath/cuda/cuda_math.h"
#include "gpu_alloc.h"
#endif

#ifdef HAVE_GD
#include "ext/gd/libgd/gd.h"

typedef struct _gd_ext_image_object {
    gdImagePtr image;
    zend_object std;
} php_gd_image_object;

static
gdImagePtr gdImagePtr_from_zobj_p(zend_object* obj)
{
    return ((php_gd_image_object *) ((char *) (obj) - XtOffsetOf(php_gd_image_object, std)))->image;
}

static
zend_always_inline php_gd_image_object* php_gd_exgdimage_from_zobj_p(zend_object* obj)
{
    return (php_gd_image_object *) ((char *) (obj) - XtOffsetOf(php_gd_image_object, std));
}

void
php_gd_assign_libgdimageptr_as_extgdimage(zval *val, gdImagePtr image)
{
    zend_class_entry* gd_image_ce = zend_lookup_class(zend_string_init("GdImage", strlen("GdImage"), 1));
    object_init_ex(val, gd_image_ce);
    php_gd_exgdimage_from_zobj_p(Z_OBJ_P(val))->image = image;
}

gdImagePtr gdImageCreateTrueColor_ (int sx, int sy)
{
    int i;
    gdImagePtr im;

    im = (gdImage *) emalloc(sizeof(gdImage));
    memset(im, 0, sizeof(gdImage));
    im->tpixels = (int **) emalloc(sizeof(int *) * sy);
    im->polyInts = 0;
    im->polyAllocated = 0;
    im->brush = 0;
    im->tile = 0;
    im->style = 0;
    for (i = 0; i < sy; i++) {
        im->tpixels[i] = (int *) ecalloc(sx, sizeof(int));
    }
    im->sx = sx;
    im->sy = sy;
    im->transparent = (-1);
    im->interlace = 0;
    im->trueColor = 1;
    /* 2.0.2: alpha blending is now on by default, and saving of alpha is
     * off by default. This allows font antialiasing to work as expected
     * on the first try in JPEGs -- quite important -- and also allows
     * for smaller PNGs when saving of alpha channel is not really
     * desired, which it usually isn't!
     */
    im->saveAlphaFlag = 0;
    im->alphaBlendingFlag = 1;
    im->thick = 1;
    im->AA = 0;
    im->cx1 = 0;
    im->cy1 = 0;
    im->cx2 = im->sx - 1;
    im->cy2 = im->sy - 1;
    im->res_x = GD_RESOLUTION;
    im->res_y = GD_RESOLUTION;
    im->interpolation = NULL;
    im->interpolation_id = GD_BILINEAR_FIXED;
    return im;
}

NDArray *
NDArray_FromGD(zval *a) {
    NDArray *rtn;
    int color_index;
    int offset_green, offset_red, offset_blue;
    int red, green, blue;
    int *i_shape = emalloc(sizeof(int) * 3);
    gdImagePtr img_ptr = gdImagePtr_from_zobj_p(Z_OBJ_P(a));
    i_shape[0] = 3;
    i_shape[1] = (int)img_ptr->sy;
    i_shape[2] = (int)img_ptr->sx;
    rtn = NDArray_Zeros(i_shape, 3, NDARRAY_TYPE_FLOAT32, NDARRAY_DEVICE_CPU);
    for (int i = 0; i < img_ptr->sy; i++) {
        for (int j = 0; j < img_ptr->sx; j++) {
            offset_red = (NDArray_STRIDES(rtn)[0]/ NDArray_ELSIZE(rtn) * 0) +
                    ((NDArray_STRIDES(rtn)[1]/ NDArray_ELSIZE(rtn)) * i) +
                    ((NDArray_STRIDES(rtn)[2]/ NDArray_ELSIZE(rtn)) * j);
            offset_green = ((NDArray_STRIDES(rtn)[0]/ NDArray_ELSIZE(rtn)) * 1) +
                    ((NDArray_STRIDES(rtn)[1]/ NDArray_ELSIZE(rtn)) * i) +
                    ((NDArray_STRIDES(rtn)[2]/ NDArray_ELSIZE(rtn)) * j);
            offset_blue = ((NDArray_STRIDES(rtn)[0]/ NDArray_ELSIZE(rtn)) * 2) +
                    ((NDArray_STRIDES(rtn)[1]/ NDArray_ELSIZE(rtn)) * i) +
                    ((NDArray_STRIDES(rtn)[2]/ NDArray_ELSIZE(rtn)) * j);
            color_index = img_ptr->tpixels[i][j];
            red = (color_index >> 16) & 0xFF;
            green = (color_index >> 8) & 0xFF;
            blue = color_index & 0xFF;
            NDArray_FDATA(rtn)[offset_red] = (float)red;
            NDArray_FDATA(rtn)[offset_blue] = (float)blue;
            NDArray_FDATA(rtn)[offset_green] = (float)green;
        }
    }
    return rtn;
}


void
NDArray_ToGD(NDArray *a, zval *output) {
    if (NDArray_NDIM(a) != 3 || NDArray_SHAPE(a)[0] != 3) {
        zend_throw_error(NULL, "Incompatible shape for image");
        return;
    }
    int color_index;
    int offset_green, offset_red, offset_blue;
    int red, green, blue;
    char *tmp_red, *tmp_blue, *tmp_green;
    gdImagePtr im = gdImageCreateTrueColor_(NDArray_SHAPE(a)[2], NDArray_SHAPE(a)[1]);
    for (int i = 0; i < im->sy; i++) {
        for (int j = 0; j < im->sx; j++) {
            offset_red = (NDArray_STRIDES(a)[0]/ NDArray_ELSIZE(a) * 0) +
                         ((NDArray_STRIDES(a)[1]/ NDArray_ELSIZE(a)) * i) +
                         ((NDArray_STRIDES(a)[2]/ NDArray_ELSIZE(a)) * j);
            offset_green = ((NDArray_STRIDES(a)[0]/ NDArray_ELSIZE(a)) * 1) +
                           ((NDArray_STRIDES(a)[1]/ NDArray_ELSIZE(a)) * i) +
                           ((NDArray_STRIDES(a)[2]/ NDArray_ELSIZE(a)) * j);
            offset_blue = ((NDArray_STRIDES(a)[0]/ NDArray_ELSIZE(a)) * 2) +
                          ((NDArray_STRIDES(a)[1]/ NDArray_ELSIZE(a)) * i) +
                          ((NDArray_STRIDES(a)[2]/ NDArray_ELSIZE(a)) * j);
            red = NDArray_FDATA(a)[offset_red];
            blue = NDArray_FDATA(a)[offset_blue];
            green = NDArray_FDATA(a)[offset_green];
            color_index = (red << 16) | (green << 8) | blue;
            im->tpixels[i][j] = color_index;
        }
    }
    php_gd_assign_libgdimageptr_as_extgdimage(output, im);
}
#endif

void apply_reduce(NDArray* result, NDArray *target, NDArray* (*operation)(NDArray*, NDArray*)) {
    NDArray* temp = operation(result, target);
    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_CPU) {
        memcpy(result->data, temp->data, result->descriptor->numElements * sizeof(float));
    } else {
#ifdef HAVE_CUBLAS
        NDArray_VMEMCPY_D2D(NDArray_DATA(temp), NDArray_DATA(result), result->descriptor->numElements * sizeof(float ));
#endif
    }
    NDArray_FREE(temp);
}

void apply_single_reduce(NDArray* result, NDArray *target, float (*operation)(NDArray*)) {
    float temp;
    float temp2;
    float tmp_result;
    int *tmp_shape = emalloc(sizeof(int));
    tmp_shape[0] = 2;
    NDArray_Print(target, 0);
    NDArray *tmp = NDArray_Zeros(tmp_shape, 2, NDARRAY_TYPE_FLOAT32, NDArray_DEVICE(result));
    if (NDArray_NDIM(target) >= 1) {
        temp = operation(target);
        temp2 = operation(result);
    } else {
        temp = NDArray_FDATA(target)[0];
        temp2 = NDArray_FDATA(result)[0];
    }
    NDArray_FDATA(tmp)[0] = temp;
    NDArray_FDATA(tmp)[1] = temp2;

    tmp_result = operation(tmp);
    php_printf("\n\n%f\n\n", tmp_result);
    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_CPU) {
        memcpy(result->data, &tmp_result, sizeof(float));
    }
}

void _reduce(int current_axis, int rtn_init, int* axis, NDArray* target, NDArray* rtn, NDArray* (*operation)(NDArray*, NDArray*)) {
    NDArray* slice;
    NDArray* rtn_slice;
    NDArrayIterator_REWIND(target);
    while(!NDArrayIterator_ISDONE(target)) {
        slice = NDArrayIterator_GET(target);
        if (axis != NULL && current_axis < *axis) {
            rtn_slice = NDArrayIterator_GET(rtn);
            _reduce(current_axis + 1, rtn_init, axis, slice, rtn_slice, operation);
            NDArrayIterator_NEXT(rtn);
            NDArrayIterator_NEXT(target);
            NDArray_FREE(rtn_slice);
            NDArray_FREE(slice);
            continue;
        }
        if (rtn_init == 0) {
            rtn_init = 1;
            if (NDArray_DEVICE(rtn) == NDARRAY_DEVICE_CPU) {
                memcpy(rtn->data, slice->data, rtn->descriptor->numElements * sizeof(float));
            }
#ifdef HAVE_CUBLAS
            if (NDArray_DEVICE(rtn) == NDARRAY_DEVICE_GPU) {
                NDArray_VMEMCPY_D2D(NDArray_DATA(slice), NDArray_DATA(rtn), rtn->descriptor->numElements * sizeof(float));
            }
#endif
            NDArrayIterator_NEXT(target);
            NDArray_FREE(slice);
            continue;
        }
        apply_reduce(rtn, slice, operation);
        NDArrayIterator_NEXT(target);
        NDArray_FREE(slice);
    }
}

void _single_reduce(int current_axis, int rtn_init, int* axis, NDArray* target, NDArray* rtn, float (*operation)(NDArray*)) {
    NDArray* slice;
    NDArray* rtn_slice;
    NDArrayIterator_REWIND(target);

    while(!NDArrayIterator_ISDONE(target)) {
        slice = NDArrayIterator_GET(target);
        if (axis != NULL && current_axis < *axis) {
            rtn_slice = NDArrayIterator_GET(rtn);
            _single_reduce(current_axis + 1, rtn_init, axis, slice, rtn_slice, operation);
            NDArrayIterator_NEXT(rtn);
            NDArrayIterator_NEXT(target);
            NDArray_FREE(rtn_slice);
            NDArray_FREE(slice);
            continue;
        }
        apply_single_reduce(rtn, slice, operation);
        NDArrayIterator_NEXT(target);
        NDArray_FREE(slice);
    }
}

/**
 * Single Reduce function
 *
 * @param array
 * @param shape
 * @param strides
 * @param ndim
 * @param axis
 * @return
 */
NDArray*
single_reduce(NDArray* array, int* axis, float (*operation)(NDArray*)) {
    char* exception_buffer[256];
    int null_axis = 0;

    if (axis == NULL) {
        null_axis = 1;
        axis = emalloc(sizeof(int));
        *axis = 0;
    }


    if (axis != NULL) {
        if (*axis >= NDArray_NDIM(array)) {
            sprintf((char *) exception_buffer, "axis %d is out of bounds for array of dimension %d", *axis,
                    NDArray_NDIM(array));
            zend_throw_error(NULL, "%s", (const char *) exception_buffer);
            return NULL;
        }
    }

    // Calculate the size and strides of the reduced output
    int out_dim = 0;
    int out_ndim = 0;

    if (axis != NULL) {
        for (int i = 0; i < NDArray_NDIM(array); i++) {
            if (i != *axis) {
                out_dim++;
            }
        }
    } else {
        out_dim = 0;
    }

    out_ndim = out_dim;
    int* out_shape = emalloc(sizeof(int) * out_ndim);

    if (axis != NULL) {
        int j = 0;
        for (int i = 0; i < NDArray_NDIM(array); i++) {
            if (i != *axis) {
                out_shape[j] = NDArray_SHAPE(array)[i];
                j++;
            }
        }
    } else {
        out_shape[0] = 1;
    }

    // Calculate the size of the reduced buffer
    int reduced_buffer_size = 1;
    for (int i = 0; i < out_dim; i++) {
        reduced_buffer_size *= out_shape[i];
    }

    // Allocate memory for the reduced buffer
    NDArray* rtn = NDArray_Zeros(out_shape, out_ndim, NDARRAY_TYPE_FLOAT32, NDArray_DEVICE(array));
    //if (reduced_buffer == NULL) {
    //    fprintf(stderr, "Memory allocation failed.\n");
    //    return;
    //}
    _single_reduce(0, 0, axis, array, rtn, operation);
    return rtn;
}

/**
 * Reduce function
 *
 * @param array
 * @param shape
 * @param strides
 * @param ndim
 * @param axis
 * @return
 */
NDArray*
reduce(NDArray* array, int* axis, NDArray* (*operation)(NDArray*, NDArray*)) {
    char* exception_buffer[256];
    int null_axis = 0;

    if (axis == NULL) {
        null_axis = 1;
        axis = emalloc(sizeof(int));
        *axis = 0;
    }


    if (axis != NULL) {
        if (*axis >= NDArray_NDIM(array)) {
            sprintf((char *) exception_buffer, "axis %d is out of bounds for array of dimension %d", *axis,
                    NDArray_NDIM(array));
            zend_throw_error(NULL, "%s", (const char *) exception_buffer);
            return NULL;
        }
    }

    // Calculate the size and strides of the reduced output
    int out_dim = 0;
    int out_ndim = 0;

    if (axis != NULL) {
        for (int i = 0; i < NDArray_NDIM(array); i++) {
            if (i != *axis) {
                out_dim++;
            }
        }
    } else {
        out_dim = 0;
    }

    out_ndim = out_dim;
    int* out_shape = emalloc(sizeof(int) * out_ndim);

    if (axis != NULL) {
        int j = 0;
        for (int i = 0; i < NDArray_NDIM(array); i++) {
            if (i != *axis) {
                out_shape[j] = NDArray_SHAPE(array)[i];
                j++;
            }
        }
    } else {
        out_shape[0] = 1;
    }

    // Calculate the size of the reduced buffer
    int reduced_buffer_size = 1;
    for (int i = 0; i < out_dim; i++) {
        reduced_buffer_size *= out_shape[i];
    }

    // Allocate memory for the reduced buffer
    NDArray* rtn = NDArray_Zeros(out_shape, out_ndim, NDARRAY_TYPE_FLOAT32, NDArray_DEVICE(array));

    //if (reduced_buffer == NULL) {
    //    fprintf(stderr, "Memory allocation failed.\n");
    //    return;
    //}
    _reduce(0, 0, axis, array, rtn, operation);

    if (null_axis == 1) {
        efree(axis);
    }
    return rtn;
}

/**
 * Free NDArray
 *
 * @param array
 */
void
NDArray_FREE(NDArray* array) {
    if (array == NULL || array->refcount == -1) {
        return;
    }

    // Decrement the reference count
    if (array->refcount > 0) {
        array->refcount--;
    }

    // If the reference count reaches zero, free the memory
    if (array->refcount == 0) {
        if (array->iterator != NULL) {
            NDArrayIterator_FREE(array);
        }

        if (array->strides != NULL) {
            efree(array->strides);
        }

        if (array->dimensions != NULL) {
            efree(array->dimensions);
        }

        if (array->data != NULL && array->base == NULL && array->descriptor->numElements > 0) {
            if (NDArray_DEVICE(array) == NDARRAY_DEVICE_CPU) {
                efree(array->data);
            } else {
#ifdef HAVE_CUBLAS
                NDArray_VFREE(array->data);
#endif
            }
        }

        if (array->base != NULL) {
            NDArray_FREE(array->base);
        }

        if (array->descriptor != NULL) {
            efree(array->descriptor);
        }
        array->refcount = -1;
        efree(array);
        array = NULL;
    }
}

/**
 * Free NDArray data buffer regardless of references
 *
 * @param target
 */
void
NDArray_FREEDATA(NDArray *target) {
    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_CPU) {
        efree(target->data);
    }
#ifdef HAVE_CUBLAS
    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_GPU) {
        NDArray_VFREE(target->data);
    }
#endif
    target->data = NULL;
}

/**
 * Print NDArray or return the print string
 *
 * @param array
 * @param do_return
 * @return
 */
char *
NDArray_Print(NDArray *array, int do_return) {
    char *str;
    if (is_type(NDArray_TYPE(array), NDARRAY_TYPE_DOUBLE64)) {
        str = print_matrix(NDArray_DDATA(array), NDArray_NDIM(array), NDArray_SHAPE(array),
                                 NDArray_STRIDES(array), NDArray_NUMELEMENTS(array), NDArray_DEVICE(array));
    }
    if (is_type(NDArray_TYPE(array), NDARRAY_TYPE_FLOAT32)) {
        str = print_matrix_float(NDArray_FDATA(array), NDArray_NDIM(array), NDArray_SHAPE(array),
                                 NDArray_STRIDES(array), NDArray_NUMELEMENTS(array), NDArray_DEVICE(array));
    }
    if (do_return == 0) {
        printf("%s", str);
        return NULL;
    }
    return str;
}

/**
 * NDArray Reduce
 *
 * @param array
 * @return
 */
NDArray*
NDArray_Reduce(NDArray *array, int axis, char* function) {

}

/**
 * Compare two NDArrays
 *
 * @param a
 * @param b
 * @return
 */
NDArray*
NDArray_Compare(NDArray *a, NDArray *b) {
    int i;
    int *rtn_shape = emalloc(sizeof(int) * NDArray_NDIM(a));
    memcpy(rtn_shape, NDArray_SHAPE(a), sizeof(int) * NDArray_NDIM(a));
    NDArray *rtn = NDArray_Zeros(rtn_shape, NDArray_NDIM(a), NDARRAY_TYPE_FLOAT32, NDArray_DEVICE(a));

    // Check if arrays have the same dimension
    if (NDArray_NDIM(a) != NDArray_NDIM(b)) {
        zend_throw_error(NULL, "Can't compare two different shape arrays");
        return NULL;
    }

    // Check if arrays are equal
    for (i = 0; i < NDArray_NDIM(a); i++) {
        if(NDArray_SHAPE(a)[i] != NDArray_SHAPE(b)[i]) {
            zend_throw_error(NULL, "Can't compare two different shape arrays");
            return NULL;
        }
    }

#ifdef HAVE_AVX2
    NDArrayIterator_REWIND(a);
    NDArrayIterator_REWIND(b);
    while(!NDArrayIterator_ISDONE(a)) {
        NDArrayIterator_NEXT(a);
        NDArrayIterator_NEXT(b);
    }
#else

#endif
    return rtn;
}


/**
 * Check whether the given array is stored contiguously
 **/
static void
_UpdateContiguousFlags(NDArray * array)
{
    int sd;
    int dim;
    int i;
    int is_c_contig = 1;

    sd = NDArray_ELSIZE(array);
    for (i = NDArray_NDIM(array) - 1; i >= 0; --i) {
        dim = NDArray_SHAPE(array)[i];

        if (NDArray_STRIDES(array)[i] != sd) {
            is_c_contig = 0;
            break;
        }
        /* contiguous, if it got this far */
        if (dim == 0) {
            break;
        }
        sd *= dim;
    }
    if (is_c_contig) {
        NDArray_ENABLEFLAGS(array, NDARRAY_ARRAY_C_CONTIGUOUS);
    }
    else {
        NDArray_CLEARFLAGS(array, NDARRAY_ARRAY_C_CONTIGUOUS);
    }
}

/**
 * Update CArray flags
 **/
void
NDArray_UpdateFlags(NDArray *array, int flagmask)
{
    if (flagmask & (NDARRAY_ARRAY_F_CONTIGUOUS | NDARRAY_ARRAY_C_CONTIGUOUS)) {
        _UpdateContiguousFlags(array);
    }
}

/**
 * @param array
 */
NDArray*
NDArray_Map(NDArray *array, ElementWiseDoubleOperation op) {
    NDArray *rtn;
    int i;
    int *new_shape = emalloc(sizeof(int) * NDArray_NDIM(array));
    memcpy(new_shape, NDArray_SHAPE(array), sizeof(int) * NDArray_NDIM(array));
    rtn = NDArray_Zeros(new_shape, NDArray_NDIM(array), NDARRAY_TYPE_FLOAT32, NDArray_DEVICE(array));

    for (i = 0; i < NDArray_NUMELEMENTS(array); i++) {
        NDArray_FDATA(rtn)[i] = op(NDArray_FDATA(array)[i]);
    }
    return rtn;
}

/**
 * @param array
 */
NDArray*
NDArray_Map1F(NDArray *array, ElementWiseFloatOperation1F op, float val1) {
    NDArray *rtn;
    int i;
    int *new_shape = emalloc(sizeof(int) * NDArray_NDIM(array));
    memcpy(new_shape, NDArray_SHAPE(array), sizeof(int) * NDArray_NDIM(array));
    rtn = NDArray_Zeros(new_shape, NDArray_NDIM(array), NDARRAY_TYPE_FLOAT32, NDArray_DEVICE(array));

    for (i = 0; i < NDArray_NUMELEMENTS(array); i++) {
        NDArray_FDATA(rtn)[i] = op(NDArray_FDATA(array)[i], val1);
    }
    return rtn;
}

/**
 * @param array
 */
NDArray*
NDArray_Map2F(NDArray *array, ElementWiseFloatOperation2F op, float val1, float val2) {
    NDArray *rtn;
    int i;
    int *new_shape = emalloc(sizeof(int) * NDArray_NDIM(array));
    memcpy(new_shape, NDArray_SHAPE(array), sizeof(int) * NDArray_NDIM(array));
    rtn = NDArray_Zeros(new_shape, NDArray_NDIM(array), NDARRAY_TYPE_FLOAT32, NDArray_DEVICE(array));

    for (i = 0; i < NDArray_NUMELEMENTS(array); i++) {
        NDArray_FDATA(rtn)[i] = op(NDArray_FDATA(array)[i], val1, val2);
    }
    return rtn;
}

/**
 * Return minimum value of NDArray
 *
 * @param target
 * @return
 */
float
NDArray_Min(NDArray *target) {
    float* array = NDArray_FDATA(target);
    int length = NDArray_NUMELEMENTS(target);
    float min = 0.f;
    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_GPU) {
#ifdef HAVE_CUBLAS
        return cuda_min_float(array, NDArray_NUMELEMENTS(target));
#else
        return -1.f;
#endif
    } else {
        min = array[0];
        for (int i = 1; i < length; i++) {
            if (array[i] < min) {
                min = array[i];
            }
        }
    }
    return min;
}

/**
 * Return maximum value of NDArray
 *
 * @param target
 * @return
 */
float
NDArray_Max(NDArray *target) {
    float max = 0.f;
    float* array = NDArray_FDATA(target);
    int length = NDArray_NUMELEMENTS(target);
    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_GPU) {
#ifdef HAVE_CUBLAS
        return cuda_max_float(array, NDArray_NUMELEMENTS(target));
#else
      return -1.f;
#endif
    } else {
        max = array[0];
        for (int i = 1; i < length; i++) {
            if (array[i] > max) {
                max = array[i];
            }
        }
    }
    return max;
}

/**
 * @param data
 * @param strides
 * @param dimensions
 * @param ndim
 * @return
 */
zval
convertToStridedArrayToPHPArray(float* data, int* strides, int* dimensions, int ndim, int elsize) {
    zval phpArray;
    int i;

    // Create a new PHP array
    //phpArray = (zval*)emalloc(sizeof(zval));
    array_init_size(&phpArray, ndim);

    for (i = 0; i < dimensions[0]; i++) {
        // If it's not the innermost dimension, recursively convert the sub-array
        if (ndim > 1) {
            int j;
            zval subArray;

            // Calculate the pointer and strides for the sub-array
            float* subData = data + (i * (strides[0]/elsize));
            int* subStrides = strides + 1;
            int* subDimensions = dimensions + 1;

            // Convert the sub-array to a PHP array
            subArray = convertToStridedArrayToPHPArray(subData, subStrides, subDimensions, ndim - 1, elsize);

            // Add the sub-array to the main array
            add_index_zval(&phpArray, i, &subArray);
        } else {
            //printf("\nNDIM: %d\n", *strides);
            // Add the scalar values to the main array
            add_index_double(&phpArray, i, *(data + (i * (*strides/elsize))));
        }
    }
    return phpArray;
}

/**
 * Convert a NDArray to PHP Array
 *
 * @return
 */
zval
NDArray_ToPHPArray(NDArray *target) {
    zval phpArray;
    phpArray = convertToStridedArrayToPHPArray(NDArray_FDATA(target), NDArray_STRIDES(target),
                                             NDArray_SHAPE(target), NDArray_NDIM(target), NDArray_ELSIZE(target));
    return phpArray;
}

/**
 * @param nda
 * @return
 */
int*
NDArray_ToIntVector(NDArray *nda) {
    double *tmp_val = emalloc(sizeof(float));
    int *vector = emalloc(sizeof(int) * NDArray_NUMELEMENTS(nda));
    for (int i = 0; i < NDArray_NUMELEMENTS(nda); i++){
        if (NDArray_DEVICE(nda) == NDARRAY_DEVICE_GPU) {
#ifdef HAVE_CUBLAS
            cudaMemcpy(tmp_val, &NDArray_FDATA(nda)[i], sizeof(float), cudaMemcpyDeviceToHost);
            vector[i] = (int) *tmp_val;
            continue;
#endif
        }
        vector[i] = (int) NDArray_FDATA(nda)[i];
    }
    efree(tmp_val);
    return vector;
}

/**
 * Transfer NDArray to GPU and return a copy
 *
 * @param target
 */
NDArray*
NDArray_ToGPU(NDArray *target)
{
#ifdef HAVE_CUBLAS
    float *tmp_gpu;
    int *new_shape;
    int n_ndim = NDArray_NDIM(target);

    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_GPU) {
        return NDArray_Copy(target, NDARRAY_DEVICE_GPU);
    }

    new_shape = emalloc(sizeof(int) * NDArray_NDIM(target));
    memcpy(new_shape, NDArray_SHAPE(target), sizeof(int) * NDArray_NDIM(target));

    NDArray *rtn = NDArray_Zeros(new_shape, n_ndim, NDARRAY_TYPE_FLOAT32, NDArray_DEVICE(target));
    rtn->device = NDARRAY_DEVICE_GPU;

    NDArray_VMALLOC((void **) &tmp_gpu, NDArray_NUMELEMENTS(target) * sizeof(float));
    cudaMemcpy(tmp_gpu, NDArray_FDATA(target), NDArray_NUMELEMENTS(target) * sizeof(float), cudaMemcpyHostToDevice);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        zend_throw_error(NULL, "Error synchronizing: %s\n", cudaGetErrorString(err));
        return NULL;
    }
    efree(rtn->data);
    rtn->data = (char*)tmp_gpu;
    return rtn;
#else
    zend_throw_error(NULL, "Unable to detect a compatible device.");
    return NULL;
#endif
}

/**
 * Transfer NDArray to CPU and return a copy
 *
 * @param target
 */
NDArray*
NDArray_ToCPU(NDArray *target)
{
    int *new_shape;
    int n_ndim = NDArray_NDIM(target);

    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_CPU) {
        return NDArray_Copy(target, NDARRAY_DEVICE_CPU);
    }

    new_shape = emalloc(sizeof(int) * NDArray_NDIM(target));
    memcpy(new_shape, NDArray_SHAPE(target), sizeof(int) * NDArray_NDIM(target));

    NDArray *rtn = NDArray_Empty(new_shape, n_ndim, NDARRAY_TYPE_FLOAT32, NDARRAY_DEVICE_CPU);
    rtn->device = NDARRAY_DEVICE_CPU;
#ifdef HAVE_CUBLAS
    cudaMemcpy(rtn->data, NDArray_FDATA(target), NDArray_NUMELEMENTS(target) * sizeof(float), cudaMemcpyDeviceToHost);
#endif
    return rtn;
}

/**
 * Return 1 if a.shape == b.shape or 0
 *
 * @param a
 * @param b
 * @return
 */
int
NDArray_ShapeCompare(NDArray *a, NDArray *b)
{
    if (NDArray_NDIM(a) != NDArray_NDIM(b)) {
        return 0;
    }

    for(int i = 0; i < NDArray_NDIM(a); i++) {
        if (NDArray_SHAPE(a)[i] != NDArray_SHAPE(b)[i]) {
            return 0;
        }
    }

    return 1;
}

/**
 * Check if two NDArray are broadcastable
 *
 * @param arr1
 * @param arr2
 * @return
 */
int
NDArray_IsBroadcastable(const NDArray* array1, const NDArray* array2) {
    if (NDArray_NDIM(array1) == 1 && NDArray_NDIM(array2) > 1) {
        if (NDArray_SHAPE(array1)[0] == NDArray_SHAPE(array2)[NDArray_NDIM(array2) - 1]) {
            return 1;
        } else {
            return 0;
        }
    }

    // Determine the maximum number of dimensions
    int maxDims = (NDArray_NDIM(array1) > NDArray_NDIM(array2)) ? NDArray_NDIM(array1) : NDArray_NDIM(array2);

    // Pad the shape arrays with 1s if necessary
    int paddedShape1[maxDims];
    int paddedShape2[maxDims];

    for (int i = 0; i < maxDims; i++) {
        paddedShape1[i] = (i < NDArray_NDIM(array1)) ? NDArray_SHAPE(array1)[i] : 1;
        paddedShape2[i] = (i < NDArray_NDIM(array2)) ? NDArray_SHAPE(array2)[i] : 1;
    }

    // Check if the modified arrays are broadcastable
    for (int i = 0; i < maxDims; i++) {
        if (paddedShape1[i] != paddedShape2[i] && paddedShape1[i] != 1 && paddedShape2[i] != 1) {
            return 0;
        }
    }

    // Arrays are broadcastable
    return 1;
}

/**
 * Broadcast NDArrays
 *
 * @todo Implement ND broadcast
 * @param a
 * @param b
 * @return
 */
NDArray*
NDArray_Broadcast(NDArray *a, NDArray *b) {
    int i;
    NDArray *src, *dst, *rtn;
    src = a;
    dst = b;
    char *tmp_p;
    if (NDArray_NDIM(a) > 2 || NDArray_NDIM(b) > 2) {
        zend_throw_error(NULL, "Broadcast shape mismatch.");
        return NULL;
    }
    if (NDArray_NDIM(a) == NDArray_NDIM(b)) {
        int all_equal = 1;
        for (i = 0; i < NDArray_NDIM(a); i++) {
            if (NDArray_SHAPE(a)[i] != NDArray_SHAPE(b)[i]) {
                all_equal = 0;
            }
        }
        if (all_equal == 1) {
            return src;
        }
    }

    if (!NDArray_IsBroadcastable(src, dst)) {
        zend_throw_error(NULL, "Broadcast shape mismatch.");
        return NULL;
    }
    rtn = NDArray_Copy(dst, NDArray_DEVICE(dst));
    char *rtn_p = NDArray_DATA(rtn);
    if (NDArray_NDIM(src) == 1 && NDArray_NDIM(dst) > 1) {
        if (NDArray_SHAPE(src)[0] == NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]) {
            if (NDArray_DEVICE(dst) == NDARRAY_DEVICE_CPU) {
                for (i = 0; i < NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]; i++) {
                    memcpy(rtn_p,
                           NDArray_FDATA(src), sizeof(float) * NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 1]);
                    rtn_p = rtn_p + (sizeof(float) * NDArray_SHAPE(src)[0]);
                }
            }
            if (NDArray_DEVICE(dst) == NDARRAY_DEVICE_GPU) {
                for (i = 0; i < NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]; i++) {
                    NDArray_VMEMCPY_D2D(NDArray_DATA(src), rtn_p,
                                        sizeof(float) * NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 1]);
                    rtn_p = rtn_p + (sizeof(float) * NDArray_SHAPE(src)[0]);
                }
            }
        }
    }
    int j;
    if (NDArray_NDIM(src) == 2 && NDArray_NDIM(dst) == 2) {
        if (NDArray_SHAPE(src)[NDArray_NDIM(dst) - 2] == NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]) {
            if (NDArray_DEVICE(dst) == NDARRAY_DEVICE_CPU) {
                for (i = 0; i < NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]; i++) {
                    for (j = 0; j < NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 1]; j++) {
                        NDArray_FDATA(rtn)[(i * NDArray_STRIDES(rtn)[NDArray_NDIM(rtn) - 2]/ NDArray_ELSIZE(rtn))+j] = NDArray_FDATA(src)[i];
                    }
                }
            }
            if (NDArray_DEVICE(dst) == NDARRAY_DEVICE_GPU) {
                for (i = 0; i < NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]; i++) {
                    for (j = 0; j < NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 1]; j++) {
                        tmp_p = (char*)(NDArray_FDATA(src) + i);
                        rtn_p = (char*)(NDArray_FDATA(rtn) + (i * NDArray_STRIDES(rtn)[NDArray_NDIM(rtn) - 2]/ NDArray_ELSIZE(rtn))+j);
                        NDArray_VMEMCPY_D2D(tmp_p, rtn_p, sizeof(float));
                    }
                }
                NDArray_Print(rtn,0);
            }
        }
        if (NDArray_SHAPE(src)[NDArray_NDIM(dst) - 1] == NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]) {
            if (NDArray_DEVICE(dst) == NDARRAY_DEVICE_CPU) {
                for (i = 0; i < NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]; i++) {
                    memcpy(rtn_p,
                           NDArray_FDATA(src), sizeof(float) * NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 1]);
                    rtn_p = rtn_p + (sizeof(float) * NDArray_SHAPE(src)[NDArray_NDIM(dst) - 1]);
                }
            }
            if (NDArray_DEVICE(dst) == NDARRAY_DEVICE_GPU) {
                for (i = 0; i < NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 2]; i++) {
                    NDArray_VMEMCPY_D2D(NDArray_DATA(src), rtn_p,
                                        sizeof(float) * NDArray_SHAPE(dst)[NDArray_NDIM(dst) - 1]);
                    rtn_p = (char *) (NDArray_FDATA(rtn) +
                                      (i * NDArray_STRIDES(rtn)[NDArray_NDIM(rtn) - 2] / NDArray_ELSIZE(rtn)) + j);
                }
            }
        }
    }
    return rtn;
}

/**
 * Get the scalar value of a NDArray
 *
 * @param a
 * @return
 */
float
NDArray_GetFloatScalar(NDArray *a) {
    if (NDArray_DEVICE(a) == NDARRAY_DEVICE_CPU) {
        return NDArray_FDATA(a)[0];
    }
#ifdef HAVE_CUBLAS
    return NDArray_VFLOAT(NDArray_DATA(a));
#endif
}

/**
 * Overwrite the values of one NDArray with the values
 * of another.
 *
 * @param target
 * @param values
 */
int
NDArray_Overwrite(NDArray *target, NDArray *values) {

    if (NDArray_NDIM(values) == 0) {
        NDArray_Fill(target, NDArray_GetFloatScalar(values));
        return 1;
    }

    if (NDArray_DEVICE(target) != NDArray_DEVICE(values)) {
        zend_throw_error(NULL, "Incompatible devices during NDArray overwrite.");
        return 0;
    }

    if (NDArray_ShapeCompare(target, values) == 0) {
        zend_throw_error(NULL, "Incompatible shapes during NDArray overwrite.");
        return 0;
    }

    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_CPU) {
        memcpy(NDArray_FDATA(target), NDArray_FDATA(values),
               sizeof(NDArray_ELSIZE(values)) * NDArray_NUMELEMENTS(values));
        return 1;
    }
#ifdef HAVE_CUBLAS
    if (NDArray_DEVICE(target) == NDARRAY_DEVICE_GPU) {
        NDArray_VMEMCPY_D2D(values->data, target->data,
                            sizeof(NDArray_ELSIZE(values)) * NDArray_NUMELEMENTS(values));
        return 1;
    }
#endif
    return 0;
}
