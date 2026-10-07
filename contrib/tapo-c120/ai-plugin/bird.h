#ifndef C120_BIRD_H
#define C120_BIRD_H
#include <stdint.h>
struct bird_tensor { unsigned width,height,channels,stride,bytes; float scale; int64_t zero; };
struct bird_box { float x1,y1,x2,y2,score; unsigned index; };
struct bird_image { const unsigned char *rgb,*y,*uv; unsigned width,height,rgb_stride,y_stride,uv_stride; };
int bird_tensor_desc(const uint32_t *desc,unsigned width,unsigned height,unsigned channels,struct bird_tensor *tensor);
int bird_prepare(const struct bird_image *image,const struct bird_tensor *tensor,int16_t *dest);
int bird_quantize(const float *rgb,unsigned pixels,const struct bird_tensor *tensor,int16_t *dest);
int bird_decode(const struct bird_tensor heads[3],const int16_t *data[3],float confidence,float nms,
                unsigned width,unsigned height,struct bird_box boxes[300]);
#endif
