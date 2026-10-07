#include "bird.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int bird_tensor_desc(const uint32_t *d,unsigned width,unsigned height,unsigned channels,struct bird_tensor *t)
{
    /* Verified MI_IPU v6 descriptor: inner stride is bytes, not logical channels. */
    float scale; int64_t zero; memcpy(&scale,d+77,4); memcpy(&zero,d+78,8);
    if (d[0]!=4 || d[1]!=2 || d[2]!=1 || d[3]!=height || d[4]!=width || d[5]!=channels ||
        d[76]<channels*2 || d[76]%2 || d[76]>((channels+7)/8)*16 ||
        d[80]<width*height*d[76] || d[80]>width*height*d[76]+64 ||
        !isfinite(scale) || scale<=0 || scale>=1 || zero < -32768 || zero>32767 || d[84]!=0) return -1;
    *t=(struct bird_tensor){width,height,channels,d[76],d[80],scale,zero}; return 0;
}
static int16_t quantize(float value,const struct bird_tensor *t)
{
    float q=value/t->scale+(float)t->zero;
    return q<=-32768?-32768:q>=32767?32767:(int16_t)q;
}
int bird_quantize(const float *rgb,unsigned pixels,const struct bird_tensor *t,int16_t *dest)
{
    if (!rgb || !dest || pixels>320*320 || !isfinite(t->scale) || t->scale<=0 || t->stride<6 || t->stride>16 || t->stride%2 || t->bytes<pixels*t->stride) return -1;
    memset(dest,0,t->bytes);
    for (unsigned i=0;i<pixels;++i) for (int c=0;c<3;++c) {
        if (!isfinite(rgb[i*3+c])) return -1;
        dest[i*(t->stride/2)+c]=quantize(rgb[i*3+c],t);
    }
    return 0;
}
static unsigned char clip(int x) { return x<0?0:x>255?255:x; }
static void pixel(const struct bird_image *im,unsigned x,unsigned y,unsigned char rgb[3])
{
    if (im->rgb) { memcpy(rgb,im->rgb+y*im->rgb_stride+x*3,3); return; }
    /* BT.601 limited-range NV12; camera colour conversion is not the offline photo path. */
    int c=(int)im->y[y*im->y_stride+x]-16;
    const unsigned char *uv=im->uv+(y/2)*im->uv_stride+(x&~1u);
    int d=(int)uv[0]-128,e=(int)uv[1]-128;
    rgb[0]=clip((298*c+409*e+128)>>8); rgb[1]=clip((298*c-100*d-208*e+128)>>8); rgb[2]=clip((298*c+516*d+128)>>8);
}
struct weights { unsigned first,count; int32_t values[16]; };
static int weights(struct weights *w,unsigned source,unsigned target)
{
    double step=(double)source/target,support=fmax(1,step);
    for (unsigned i=0;i<target;++i) {
        double center=(i+.5)*step,sum=0;
        int first=(int)(center-support+.5),last=(int)(center+support+.5);
        if (first<0) first=0;
        if (last>(int)source) last=source;
        if (last<=first || last-first>16) return -1;
        w[i].first=first; w[i].count=last-first;
        for (int p=first;p<last;++p) sum+=fmax(0,1-fabs((p+.5-center)/support));
        if (!sum) return -1;
        for (int p=first;p<last;++p) w[i].values[p-first]=(int32_t)floor(fmax(0,1-fabs((p+.5-center)/support))/sum*(1<<22)+.5);
    }
    return 0;
}
int bird_prepare(const struct bird_image *im,const struct bird_tensor *t,int16_t *dest)
{
    if (!im || !dest || !im->width || !im->height || im->width>1600 || im->height>900 || t->width!=320 || t->height!=320 || t->channels!=3 ||
        t->stride<6 || t->stride>16 || t->stride%2 || t->bytes<320*320*t->stride || !isfinite(t->scale) || t->scale<=0 ||
        (im->rgb?im->rgb_stride<im->width*3:!im->y || !im->uv || im->width%2 || im->height%2 || im->y_stride<im->width || im->uv_stride<im->width)) return -1;
    double gain=fmin(320.0/im->width,320.0/im->height);
    unsigned w=(unsigned)nearbyint(im->width*gain),h=(unsigned)nearbyint(im->height*gain);
    if (!w || !h) return -1;
    unsigned left=(unsigned)nearbyint((320-w)/2.0-.1),top=(unsigned)nearbyint((320-h)/2.0-.1);
    struct weights *wx=calloc(w,sizeof(*wx)),*wy=calloc(h,sizeof(*wy));
    unsigned char *horizontal=malloc(w*im->height*3);
    if (!wx || !wy || !horizontal || weights(wx,im->width,w) || weights(wy,im->height,h)) { free(wx); free(wy); free(horizontal); return -1; }
    memset(dest,0,t->bytes);
    int16_t pad=quantize(114/255.0f,t); unsigned pitch=t->stride/2;
    for (unsigned i=0;i<320*320;++i) for (int c=0;c<3;++c) dest[i*pitch+c]=pad;
    /* Separable antialiased bilinear resize; round each 8-bit pass like the photo baseline. */
    for (unsigned y=0;y<im->height;++y) for (unsigned x=0;x<w;++x) {
        int32_t sum[3]={1<<21,1<<21,1<<21};
        for (unsigned k=0;k<wx[x].count;++k) { unsigned char p[3]; pixel(im,wx[x].first+k,y,p); for (int c=0;c<3;++c) sum[c]+=p[c]*wx[x].values[k]; }
        for (int c=0;c<3;++c) horizontal[(y*w+x)*3+c]=clip(sum[c]>>22);
    }
    for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) for (int c=0;c<3;++c) {
        int32_t sum=1<<21; for (unsigned k=0;k<wy[y].count;++k) sum+=horizontal[((wy[y].first+k)*w+x)*3+c]*wy[y].values[k];
        dest[((y+top)*320+x+left)*pitch+c]=quantize(clip(sum>>22)/255.0f,t);
    }
    free(wx); free(wy); free(horizontal); return 0;
}
static float sigmoid(float x) { return 1/(1+expf(-x)); }
static float value(const int16_t *p,unsigned i,const struct bird_tensor *t) { return ((float)p[i]-(float)t->zero)*t->scale; }
static int order(const void *a,const void *b)
{
    const struct bird_box *x=a,*y=b;
    return x->score>y->score?-1:x->score<y->score?1:x->index<y->index?-1:x->index>y->index;
}
static float iou(const struct bird_box *a,const struct bird_box *b)
{
    float intersection=fmaxf(0,fminf(a->x2,b->x2)-fmaxf(a->x1,b->x1))*fmaxf(0,fminf(a->y2,b->y2)-fmaxf(a->y1,b->y1));
    float area=(a->x2-a->x1)*(a->y2-a->y1)+(b->x2-b->x1)*(b->y2-b->y1)-intersection;
    return area>0?intersection/area:0;
}
int bird_decode(const struct bird_tensor heads[3],const int16_t *data[3],float confidence,float nms,unsigned width,unsigned height,struct bird_box boxes[300])
{
    static const float anchors[3][3][2]={{{10,13},{16,30},{33,23}},{{30,61},{62,45},{59,119}},{{116,90},{156,198},{373,326}}};
    if (!width || !height || !isfinite(confidence) || confidence<.25 || confidence>.99 || !isfinite(nms) || nms<.05 || nms>.95) return -1;
    struct bird_box *candidates=malloc(6300*sizeof(*candidates)); if (!candidates) return -1;
    unsigned count=0,base=0;
    for (unsigned head=0;head<3;++head) {
        const struct bird_tensor *t=&heads[head]; unsigned size=40>>head,stride=8<<head;
        if (!data[head] || t->width!=size || t->height!=size || t->channels!=255 || t->stride<510 || t->stride%2 || t->bytes<size*size*t->stride || !isfinite(t->scale) || t->scale<=0) { free(candidates); return -1; }
        for (unsigned a=0;a<3;++a) for (unsigned y=0;y<size;++y) for (unsigned x=0;x<size;++x) {
            const int16_t *p=data[head]+(y*size+x)*(t->stride/2)+a*85;
            float objectness=sigmoid(value(p,4,t)); if (objectness<=confidence) continue;
            unsigned best=0; for (unsigned c=1;c<80;++c) if (p[5+c]>p[5+best]) best=c;
            if (best!=14) continue;
            float score=objectness*sigmoid(value(p,19,t)); if (score<=confidence) continue;
            float cx=(sigmoid(value(p,0,t))*2-.5f+x)*stride,cy=(sigmoid(value(p,1,t))*2-.5f+y)*stride;
            float w=powf(sigmoid(value(p,2,t))*2,2)*anchors[head][a][0],h=powf(sigmoid(value(p,3,t))*2,2)*anchors[head][a][1];
            candidates[count++]=(struct bird_box){cx-w/2,cy-h/2,cx+w/2,cy+h/2,score,base+a*size*size+y*size+x};
        }
        base+=3*size*size;
    }
    qsort(candidates,count,sizeof(*candidates),order); unsigned kept=0;
    for (unsigned i=0;i<count && kept<300;++i) {
        int suppress=0; for (unsigned j=0;j<kept;++j) if (iou(&candidates[i],&boxes[j])>nms) { suppress=1; break; }
        if (!suppress) boxes[kept++]=candidates[i];
    }
    float gain=fminf(320.0f/width,320.0f/height);
    float px=(320-nearbyintf(width*gain))/2,py=(320-nearbyintf(height*gain))/2;
    for (unsigned i=0;i<kept;++i) {
        struct bird_box *b=&boxes[i];
        b->x1=fminf(width,fmaxf(0,(b->x1-px)/gain))/width; b->x2=fminf(width,fmaxf(0,(b->x2-px)/gain))/width;
        b->y1=fminf(height,fmaxf(0,(b->y1-py)/gain))/height; b->y2=fminf(height,fmaxf(0,(b->y2-py)/gain))/height;
    }
    free(candidates); return kept;
}
