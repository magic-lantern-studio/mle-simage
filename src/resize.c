/*
 *		Filtered Image Rescaling
 *
 *		  by Dale Schumacher 
 */

/*
 * Slightly modified by pederb to be able to use it
 * in simage.
 *
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

/* Need to include this so the compiler knows that the simage_resize()
   method should be defined with __declspec(dllexport) under
   MSWindows. */
#include <simage.h>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif /* !M_PI */

typedef struct {
  int xsize;            /* horizontal size of the image in Pixels */
  int ysize;            /* vertical size of the image in Pixels */
  int bpp;              /* bytes per pixel */
  unsigned char * data; /* pointer to first scanline of image */
  int span;             /* byte offset between two scanlines */
} Image;

static void
get_row(unsigned char * row, Image * image, int y)
{
  assert(y >= 0);
  assert(y < image->ysize);
  
  memcpy(row,
         image->data + (y * image->span),
         (image->bpp * image->xsize));
}

static void
get_column(unsigned char * column, Image * image, int x)
{
  int i, j, d, bpp, ysize;
  unsigned char * p;
  
  assert(x >= 0);
  assert(x < image->xsize);

  d = image->span;
  bpp = image->bpp;
  ysize = image->ysize;
  for(i = 0, p = image->data + x * bpp; i < ysize; p += d, i++) {
    for (j = 0; j < bpp; j++) {
      *column++ = p[j];
    }
  }
}

static void
put_pixel(Image * image, int x, int y, float * data)
{
  int i, bpp;
  unsigned char * p;
  float val;

  assert(x >= 0);
  assert(x < image->xsize);
  assert(y >= 0);
  assert(y < image->ysize);
  
  bpp = image->bpp;
  p = image->data + image->span * y + x * bpp;
  for (i = 0; i < bpp; i++) {
    val = data[i];
    if (val < 0.0) val = 0.0;
    else if (val > 255.0) val = 255.0;
    *p++ = (unsigned char) val; 
  }
}


/*
 *	filter function definitions
 */

#define	filter_support		(1.0)

static float
filter(float t)
{
  /* f(t) = 2|t|^3 - 3|t|^2 + 1, -1 <= t <= 1 */
  if(t < 0.0) t = -t;
  if(t < 1.0) return((2.0 * t - 3.0) * t * t + 1.0);
  return(0.0);
}

#define	box_support		(0.5)

static float
box_filter(float t)
{
  if((t > -0.5) && (t <= 0.5)) return(1.0);
  return(0.0);
}

#define	triangle_support	(1.0)

static float
triangle_filter(float t)
{
  if(t < 0.0) t = -t;
  if(t < 1.0) return(1.0 - t);
  return(0.0);
}

#define	bell_support		(1.5)

static float
bell_filter(float t)		/* box (*) box (*) box */
{
  if(t < 0) t = -t;
  if(t < .5) return(.75 - (t * t));
  if(t < 1.5) {
    t = (t - 1.5);
    return(.5 * (t * t));
  }
  return(0.0);
}

#define	B_spline_support	(2.0)

static float
B_spline_filter(float t)  /* box (*) box (*) box (*) box */
{
  float tt;
  
  if(t < 0) t = -t;
  if(t < 1) {
    tt = t * t;
    return((.5 * tt * t) - tt + (2.0 / 3.0));
  } else if(t < 2) {
    t = 2 - t;
    return((1.0 / 6.0) * (t * t * t));
  }
  return(0.0);
}

static float
sinc(float x)
{
  x *= M_PI;
  if(x != 0) return(sin(x) / x);
  return(1.0);
}

#define	Lanczos3_support	(3.0)

static float
Lanczos3_filter(float t)
{
  if(t < 0) t = -t;
  if(t < 3.0) return(sinc(t) * sinc(t/3.0));
  return(0.0);
}

#define	Mitchell_support	(2.0)

#define	B	(1.0 / 3.0)
#define	C	(1.0 / 3.0)

static float
Mitchell_filter(float t)
{
  float tt;

  tt = t * t;
  if(t < 0.0) t = -t;
  if(t < 1.0) {
    t = (((12.0 - 9.0 * B - 6.0 * C) * (t * tt))
         + ((-18.0 + 12.0 * B + 6.0 * C) * tt)
         + (6.0 - 2.0 * B));
    return (t / 6.0);
  } 
  else if (t < 2.0) {
    t = (((-1.0 * B - 6.0 * C) * (t * tt))
         + ((6.0 * B + 30.0 * C) * tt)
         + ((-12.0 * B - 48.0 * C) * t)
         + (8.0 * B + 24.0 * C));
    return (t / 6.0);
  }
  return(0.0);
}

/*
 *	image rescaling routine
 */

typedef struct {
  int pixel;
  float weight;
} CONTRIB;

typedef struct {
  int	n;		/* number of contributors */
  CONTRIB * p;		/* pointer to list of contributions */
} CLIST;

static Image * 
new_image(int xsize, int ysize, int bpp, unsigned char * data)
{
  Image * img = (Image*) malloc(sizeof(Image));
  img->xsize = xsize;
  img->ysize = ysize;
  img->bpp = bpp;
  img->span = xsize * bpp;
  img->data = data;
  if (data == NULL) img->data = (unsigned char*) malloc(img->span*img->ysize);
  return img;
}

static void
zoom(Image * dst,               /* destination image structure */
     Image * src,               /* source image structure */
     float (*filterf)(float), /* filter function */
     float fwidth)             /* filter width (support) */
{
  CLIST * contrib;
  Image * tmp;                  /* intermediate image */
  float xscale, yscale;	/* zoom scale factors */
  int i, j, k, b;			/* loop variables */
  int n;			/* pixel number */
  float center, left, right;	/* filter calculation variables */
  float width, fscale, weight;	/* filter calculation variables */
  unsigned char * raster;	/* a row or column of pixels */
  float pixel[4];              /* one pixel */
  int bpp;
  unsigned char * dstptr;
  int dstxsize, dstysize;

  bpp = src->bpp;
  dstxsize = dst->xsize;
  dstysize = dst->ysize;

  /* create intermediate image to hold horizontal zoom */
  tmp = new_image(dstxsize, src->ysize, dst->bpp, NULL);
  xscale = (float) dstxsize / (float) src->xsize;
  yscale = (float) dstysize / (float) src->ysize;

  /* pre-calculate filter contributions for a row */
  contrib = (CLIST *)calloc(dstxsize, sizeof(CLIST));
  if(xscale < 1.0) {
    width = fwidth / xscale;
    fscale = 1.0 / xscale;
    for(i = 0; i < dstxsize; i++) {
      contrib[i].n = 0;
      contrib[i].p = (CONTRIB *)calloc((int) (width * 2 + 1),
                                       sizeof(CONTRIB));
      center = (float) i / xscale;
      left = ceil(center - width);
      right = floor(center + width);
      for(j = left; j <= right; j++) {
        weight = center - (float) j;
        weight = (*filterf)(weight / fscale) / fscale;
        if(j < 0) {
          n = -j;
        } 
        else if(j >= src->xsize) {
          n = (src->xsize - j) + src->xsize - 1;
        } 
        else {
          n = j;
        }
        k = contrib[i].n++;
        contrib[i].p[k].pixel = n*bpp;
        contrib[i].p[k].weight = weight;
      }
    }
  } 
  else {
    for(i = 0; i < dstxsize; i++) {
      contrib[i].n = 0;
      contrib[i].p = (CONTRIB *)calloc((int) (fwidth * 2 + 1),
                                       sizeof(CONTRIB));
      center = (float) i / xscale;
      left = ceil(center - fwidth);
      right = floor(center + fwidth);
      for(j = left; j <= right; j++) {
        weight = center - (float) j;
        weight = (*filterf)(weight);
        if(j < 0) {
          n = -j;
        } 
        else if(j >= src->xsize) {
          n = (src->xsize - j) + src->xsize - 1;
        } 
        else {
          n = j;
        }
        k = contrib[i].n++;
        contrib[i].p[k].pixel = n*bpp;
        contrib[i].p[k].weight = weight;
      }
    }
  }
  
  /* apply filter to zoom horizontally from src to tmp */
  raster = (unsigned char *)calloc(src->xsize, src->bpp);

  dstptr = tmp->data;

  for(k = 0; k < tmp->ysize; k++) {
    get_row(raster, src, k);
    for(i = 0; i < tmp->xsize; i++) {
      for (b = 0; b < bpp; b++) pixel[b] = 0.0;
      for(j = 0; j < contrib[i].n; j++) {
        for (b = 0; b < bpp; b++) {
          pixel[b] += raster[contrib[i].p[j].pixel+b]
            * contrib[i].p[j].weight;
        }
      }
#if 1 /* obsoleted 2001-11-18 pederb. Too slow */
      put_pixel(tmp, i, k, pixel);
#else /* new code */
      for (b = 0; b < bpp; b++) {
        float val = pixel[b];
        if (val < 0.0) val = 0.0;
        else if (val > 255.0) val = 255.0;
        *dstptr++ = (unsigned char ) val;
      }
#endif /* new, faster code */
    }
  }
  free(raster);
  
  /* free the memory allocated for horizontal filter weights */
  for(i = 0; i < tmp->xsize; i++) {
    free(contrib[i].p);
  }
  free(contrib);
  
  /* pre-calculate filter contributions for a column */
  contrib = (CLIST *)calloc(dstysize, sizeof(CLIST));
  if(yscale < 1.0) {
    width = fwidth / yscale;
    fscale = 1.0 / yscale;
    for(i = 0; i < dstysize; i++) {
      contrib[i].n = 0;
      contrib[i].p = (CONTRIB *)calloc((int) (width * 2 + 1),
                                       sizeof(CONTRIB));
      center = (float) i / yscale;
      left = ceil(center - width);
      right = floor(center + width);
      for(j = left; j <= right; j++) {
        weight = center - (float) j;
        weight = (*filterf)(weight / fscale) / fscale;
        if(j < 0) {
          n = -j;
        } 
        else if(j >= tmp->ysize) {
          n = (tmp->ysize - j) + tmp->ysize - 1;
        } 
        else {
          n = j;
        }
        k = contrib[i].n++;
        contrib[i].p[k].pixel = n*bpp;
        contrib[i].p[k].weight = weight;
      }
    }
  } 
  else {
    for(i = 0; i < dstysize; i++) {
      contrib[i].n = 0;
      contrib[i].p = (CONTRIB *)calloc((int) (fwidth * 2 + 1),
                                       sizeof(CONTRIB));
      center = (float) i / yscale;
      left = ceil(center - fwidth);
      right = floor(center + fwidth);
      for(j = left; j <= right; j++) {
        weight = center - (float) j;
        weight = (*filterf)(weight);
        if(j < 0) {
          n = -j;
        } 
        else if(j >= tmp->ysize) {
          n = (tmp->ysize - j) + tmp->ysize - 1;
        } 
        else {
          n = j;
        }
        k = contrib[i].n++;
        contrib[i].p[k].pixel = n*bpp;
        contrib[i].p[k].weight = weight;
      }
    }
  }
  
  /* apply filter to zoom vertically from tmp to dst */
  raster = (unsigned char *) calloc(tmp->ysize, tmp->bpp);
  for(k = 0; k < dstxsize; k++) {
    get_column(raster, tmp, k);
    dstptr = dst->data + k * bpp;
    for(i = 0; i < dstysize; i++) {
      for (b = 0; b < bpp; b++) pixel[b] = 0.0;
      for(j = 0; j < contrib[i].n; ++j) {
        for (b = 0; b < bpp; b++) { 
          pixel[b] += raster[contrib[i].p[j].pixel+b]
            * contrib[i].p[j].weight;
        }
      }
#if 1 /* obsoleted 2001-11-18 pederb. Too slow */
      put_pixel(dst, k, i, pixel);
#else /* new code */
      for (b = 0; b < bpp; b++) {
        float val = pixel[b];
        if (val < 0.0) val = 0.0;
        else if (val > 255.0) val = 255.0;
        dstptr[b] = (unsigned char) val;
      }
#endif /* new, faster code */
      dstptr += bpp * dstxsize;
    }
  }

  free(raster);
  
  /* free the memory allocated for vertical filter weights */
  for(i = 0; i < dstysize; ++i) {
    free(contrib[i].p);
  }
  free(contrib);
  free(tmp->data);
  free(tmp);
}

/*
 * a pretty lame resize-function
 */
static unsigned char *
simage_resize_fast(unsigned char *src, int width,
                   int height, int num_comp,
                   int newwidth, int newheight)
{
  float sx, sy, dx, dy;
  int src_bpr, dest_bpr, xstop, ystop, x, y, offset, i;
  unsigned char *dest = 
    (unsigned char*) malloc(newwidth*newheight*num_comp);
  
  dx = ((float)width)/((float)newwidth);
  dy = ((float)height)/((float)newheight);
  src_bpr = width * num_comp;
  dest_bpr = newwidth * num_comp;
  
  sy = 0.0f;
  ystop = newheight * dest_bpr;
  xstop = newwidth * num_comp;   
  for (y = 0; y < ystop; y += dest_bpr) {
    sx = 0.0f;
    for (x = 0; x < xstop; x += num_comp) {
      offset = ((int)sy)*src_bpr + ((int)sx)*num_comp;
      for (i = 0; i < num_comp; i++) dest[x+y+i] = src[offset+i];
      sx += dx;
    }
    sy += dy;
  }
  return dest;
}


// FIXME: pederb suspects that MSVC++ v6 mis-compiles some code in the
// resize functionality -- which again causes a crash whenever (?) a
// resize is attempted with a simage-library compiled in release mode.
// This sounds very nasty with regard to application programmers using
// Coin / simage, and should be investigated ASAP. 20010904 mortene.

// UPDATE: I haven't observed any crashes in the resize functionality
// for several years. That goes for debug builds as well as for
// release builds. Perhaps the bug has been fixed in the latest
// service pack, SP5 (which I'm using). 20031210 thammer.

// FIXME: methinks the type of the first argument should have been
// ``const unsigned char *''. Can't change it now, though, as it'd
// probably break ABI compatibility (?). Wait for simage2. 20010809 mortene.

unsigned char *
simage_resize(unsigned char * src, int width,
	      int height, int num_comp,
	      int newwidth, int newheight)
{
  unsigned char * dstdata;
  Image * srcimg, * dstimg;

#if 0 /* for comparing speed of resize functions */
  return simage_resize_fast(src, width,
                            height, num_comp,
                            newwidth, newheight);
#endif /* testing only */
  srcimg = new_image(width, height, num_comp, src);
  dstimg = new_image(newwidth, newheight, num_comp, NULL);

  /* Using the bell filter as default */
  zoom(dstimg, srcimg, bell_filter, bell_support);

  dstdata = dstimg->data;
  free(srcimg);
  free(dstimg);
  return dstdata;
}
