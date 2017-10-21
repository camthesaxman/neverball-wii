#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "wiigl.h"
#include "log.h"

#define DEBUG

#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof(*arr))
#define DEFAULT_FIFO_SIZE (512 * 1024)

static GXRModeObj *videoMode;
static void *frameBuffers[2] = {NULL, NULL};
static int frameBufferNum = 0;
static bool initialized = false;

static struct
{
    u32 alphaTest:1;
    u32 blend:1;
    u32 clipPlanes:6;
    u32 colorMaterial:1;
    u32 cullFace:1;
    u32 depthTest:1;
    u32 lights:8;
    u32 lighting:1;
    u32 normalize:1;
    u32 polygonOffsetFill:1;
    u32 pointSprite:1;
    u32 stencilTest:1;
    u32 texture2d:1;
    u32 textureGenS:1;
    u32 textureGenT:1;
} serverEnabled;

static struct
{
    u32 colorArray:1;
    u32 indexArray:1;
    u32 normalArray:1;
    u32 textureCoordArray:1;
    u32 vertexArray:1;
} clientEnabled;

struct Buffer
{
    void *data;
    u32 size;
};

struct Buffer *boundBuffers[2];

struct VtxDesc
{
    int components;
    int format;
    int stride;
    const void *pointer;
};

static struct VtxDesc posDesc;
static struct VtxDesc colorDesc;
static struct VtxDesc texCoordDesc;
static struct VtxDesc nrmDesc;

struct Texture
{
    GXTexObj texObj;
    bool initialized;
    void *imgBuffer;
    u8 magFilter;
    u8 minFilter;
};

struct Texture *boundTexture;

#define MTX_STACK_LIMIT 16

struct MatrixStack
{
    // It's okay to pass a Mtx44 to a function that requires Mtx, but NOT the other way around.
    Mtx44 stack[MTX_STACK_LIMIT];
    int stackPos;
};

static struct MatrixStack modelviewMtxStack;
static struct MatrixStack projMtxStack;
static struct MatrixStack textureMtxStack;
static struct MatrixStack *currMtxStack;
static GLenum matrixMode;
static f32 (*currMtx)[4];
#define CURR_MATRIX currMtxStack->stack[currMtxStack->stackPos]

static u8 zEnable;
static u8 zFunc;
static u8 zUpdate;
static u8 cullMode;

static GXLightObj lightObj[8];

static float polyOffsFactor;
static float polyOffsUnits;

static void fatal_error(const char *msgfmt, ...)
{
    va_list args;

    va_start(args, msgfmt);
    vprintf(msgfmt, args);
    va_end(args);
    exit(1);
}

static int gl_enum_to_gx(GLenum n)
{
    switch (n)
    {
        // Types
        case GL_BYTE:           return GX_S8;
        case GL_UNSIGNED_BYTE:  return GX_U8;
        case GL_SHORT:          return GX_S16;
        case GL_UNSIGNED_SHORT: return GX_U16;
        case GL_FLOAT:          return GX_F32;

        // Primitives
        case GL_POINTS:         return GX_POINTS;
        case GL_LINE_STRIP:     return GX_LINESTRIP;
        case GL_TRIANGLES:      return GX_TRIANGLES;
        case GL_TRIANGLE_STRIP: return GX_TRIANGLESTRIP;
        case GL_QUADS:          return GX_QUADS;

        // Depth functions
        case GL_NEVER:          return GX_NEVER;
        case GL_LESS:           return GX_LESS;
        case GL_EQUAL:          return GX_EQUAL;
        case GL_LEQUAL:         return GX_LEQUAL;
        case GL_GREATER:        return GX_GREATER;
        case GL_NOTEQUAL:       return GX_NEQUAL;
        case GL_GEQUAL:         return GX_GEQUAL;
        case GL_ALWAYS:         return GX_ALWAYS;

        // Texture wrap modes
        case GL_CLAMP_TO_EDGE:   return GX_CLAMP;
        case GL_MIRRORED_REPEAT: return GX_MIRROR;
        case GL_REPEAT:          return GX_REPEAT;

        // Texture filters
        case GL_NEAREST:                return GX_NEAR;
        case GL_LINEAR:                 return GX_LINEAR;
        case GL_NEAREST_MIPMAP_NEAREST: return GX_NEAR_MIP_NEAR;
        case GL_LINEAR_MIPMAP_NEAREST:  return GX_LIN_MIP_NEAR;
        case GL_NEAREST_MIPMAP_LINEAR:  return GX_NEAR_MIP_LIN;
        case GL_LINEAR_MIPMAP_LINEAR:   return GX_LIN_MIP_LIN;

        // Cull mode
        // (OpenGL considers counterclockwise polygons front-facing while GX is the opposite)
        case GL_FRONT:          return GX_CULL_BACK;
        case GL_BACK:           return GX_CULL_FRONT;
        case GL_FRONT_AND_BACK: return GX_CULL_ALL;
#ifdef DEBUG
        default:
            fatal_error("unknown GL enum: %i\n", n);
#endif
    }
    return 0;
}

static u32 round_up(u32 number, u32 multiple)
{
    return ((number + multiple - 1) / multiple) * multiple;
}

static void flush_mem_range(const void *mem, u32 length)
{
    DCStoreRange((void *)mem, length);
}

static void initialize_video(void)
{
    void *gpFifo;
    f32 yScale;
    
    // Initialize video subsystem
    VIDEO_Init();
    
    // Set preferred settings
    videoMode = VIDEO_GetPreferredMode(NULL);
    VIDEO_Configure(videoMode);
    
    // Allocate framebuffers for double buffering
    frameBuffers[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(videoMode));
    frameBuffers[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(videoMode));
    VIDEO_SetNextFramebuffer(frameBuffers[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    
    //Allocate the GPU FIFO buffer
    gpFifo = memalign(32, DEFAULT_FIFO_SIZE);
    memset(gpFifo, 0, DEFAULT_FIFO_SIZE);
    GX_Init(gpFifo, DEFAULT_FIFO_SIZE);
    
    GX_SetViewport(0.0, 0.0, videoMode->fbWidth, videoMode->efbHeight, 0.0, 1.0);  //Use the entire EFB for rendering
    yScale = GX_GetYScaleFactor(videoMode->efbHeight, videoMode->xfbHeight);
    GX_SetDispCopyYScale(yScale);  //Make the TV output look like the EFB
    GX_SetScissor(0, 0, videoMode->fbWidth, videoMode->efbHeight);
    
    GX_SetDispCopySrc(0, 0, videoMode->fbWidth, videoMode->efbHeight);  //EFB -> XFB copy dimensions
    GX_SetDispCopyDst(videoMode->fbWidth, videoMode->xfbHeight);
    GX_SetCopyFilter(videoMode->aa, videoMode->sample_pattern, GX_TRUE, videoMode->vfilter);
    //Turn on field mode if video is interlaced
    if (videoMode->viHeight == 2 * videoMode->xfbHeight)
        GX_SetFieldMode(videoMode->field_rendering, GX_ENABLE);
    else
        GX_SetFieldMode(videoMode->field_rendering, GX_DISABLE);
    
    GX_CopyDisp(frameBuffers[frameBufferNum], GX_TRUE);  //Draw first frame
    GX_SetDispCopyGamma(GX_GM_1_0);
    
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);
    initialized = true;
}

void wiigl_create_context(void)
{
    if (!initialized)
        initialize_video();

    memset(&clientEnabled, 0, sizeof(clientEnabled));
    memset(&serverEnabled, 0, sizeof(serverEnabled));
    
    boundTexture = NULL;
    
    for (u32 i = 0; i < ARRAY_COUNT(boundBuffers); i++)
        boundBuffers[i] = NULL;
    
    memset(&modelviewMtxStack, 0, sizeof(modelviewMtxStack));
    memset(&projMtxStack, 0, sizeof(projMtxStack));
    memset(&textureMtxStack, 0, sizeof(textureMtxStack));
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();

    posDesc.pointer = NULL;
    colorDesc.pointer = NULL;
    texCoordDesc.pointer = NULL;
    nrmDesc.pointer = NULL;
    
    glDisable(GL_TEXTURE_2D);
    cullMode = GX_CULL_FRONT;
    zEnable = GX_FALSE;
    zFunc = GX_LEQUAL;
    zUpdate = GX_TRUE;
    GX_SetZMode(zEnable, zFunc, zUpdate);
    glMatrixMode(GL_MODELVIEW);
    
    GX_SetNumTevStages(1);
    GX_SetNumTexGens(1);
    GX_SetNumChans(1);
    
    /*
    static GXLightObj lobj;
    GX_InitLightPos(&lobj, 5, 5, 5);
    GX_InitLightColor(&lobj, (GXColor){255, 255, 255, 255});
    GX_LoadLightObj(&lobj, GX_LIGHT0);
    */
    // Light TEV stage
    //GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);  // Light color
    //GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_RASC, GX_CC_ONE, GX_CC_CPREV, GX_CC_ZERO);  // Blend
    GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_RASC, GX_CC_CPREV, GX_CC_ZERO); // Modulate
    GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    //GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_APREV, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CA_RASA, GX_CA_APREV, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR1A1);
    
    GX_SetChanAmbColor(GX_COLOR1A1, (GXColor){128, 128, 128, 255});
    GX_SetChanMatColor(GX_COLOR1A1, (GXColor){255, 255, 255, 255});
    GX_SetChanCtrl(GX_COLOR1A1, GX_ENABLE, GX_SRC_REG,GX_SRC_REG,GX_LIGHT0,GX_DF_CLAMP,GX_AF_NONE);
    
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEXCOORD0, GX_TEXMTX0);
    
    //GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    
    GX_ClearVtxDesc();
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
    GXColor color = {
        .r = red * 255,
        .g = green * 255,
        .b = blue * 255,
        .a = alpha * 255,
    };
    GX_SetCopyClear(color, 0x00FFFFFF);
}

/*
void wiigl_swap_buffers(void)
{
    GX_DrawDone();
    VIDEO_WaitVSync();
    GX_CopyDisp(frameBuffers[0], GX_TRUE);
}
*/

void wiigl_swap_buffers(void)
{
    frameBufferNum ^= 1;  //Switch to other framebuffer
    GX_DrawDone();
    VIDEO_WaitVSync();
    GX_CopyDisp(frameBuffers[frameBufferNum], GX_TRUE);
    VIDEO_SetNextFramebuffer(frameBuffers[frameBufferNum]);
    //VIDEO_Flush();
    //VIDEO_WaitVSync();
}

void glEnable(GLenum cap)
{
    switch (cap)
    {
        case GL_ALPHA_TEST:
            serverEnabled.alphaTest = true;
            break;
        case GL_BLEND:
            serverEnabled.blend = true;
            break;
        case GL_CLIP_PLANE0:
        case GL_CLIP_PLANE1:
        case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3:
        case GL_CLIP_PLANE4:
        case GL_CLIP_PLANE5:
            serverEnabled.clipPlanes |= (1 << (cap - GL_CLIP_PLANE0));
            break;
        case GL_COLOR_MATERIAL:
            serverEnabled.colorMaterial = true;
            break;
        case GL_CULL_FACE:
            serverEnabled.cullFace = true;
            GX_SetCullMode(cullMode);
            break;
        case GL_DEPTH_TEST:
            serverEnabled.depthTest = true;
            zEnable = GX_TRUE;
            GX_SetZMode(zEnable, zFunc, zUpdate);
            break;
        case GL_LIGHT0:
        case GL_LIGHT1:
        case GL_LIGHT2:
        case GL_LIGHT3:
        case GL_LIGHT4:
        case GL_LIGHT5:
        case GL_LIGHT6:
        case GL_LIGHT7:
            serverEnabled.lights |= (1 << (cap - GL_LIGHT0));
            GX_SetChanCtrl(GX_COLOR1A1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, serverEnabled.lights, GX_DF_CLAMP, GX_AF_NONE);
            break;
        case GL_LIGHTING:
            serverEnabled.lighting = true;
            GX_SetNumChans(2);
            GX_SetNumTevStages(2);
            break;
        case GL_NORMALIZE:
            serverEnabled.normalize = true;
            break;
        case GL_POLYGON_OFFSET_FILL:
            serverEnabled.polygonOffsetFill = true;
            // HACK! manipulate depth test to emulate this
            //GX_SetZMode(zEnable, GX_ALWAYS, zUpdate);
            break;
        case GL_POINT_SPRITE:
            serverEnabled.pointSprite = true;
            break;
        case GL_STENCIL_TEST:
            serverEnabled.stencilTest = true;
            break;
        case GL_TEXTURE_2D:
            serverEnabled.texture2d = true;
            break;
        case GL_TEXTURE_GEN_S:
            serverEnabled.textureGenS = true;
            break;
        case GL_TEXTURE_GEN_T:
            serverEnabled.textureGenT = true;
            break;
#ifdef DEBUG
        default:
            printf("cap = %i\n", cap);
            fatal_error("glEnable: unknown capability %i\n", cap);
#endif
    }
}

void glDisable(GLenum cap)
{
    switch (cap)
    {
        case GL_ALPHA_TEST:
            serverEnabled.alphaTest = false;
            break;
        case GL_BLEND:
            serverEnabled.blend = false;
            break;
        case GL_CLIP_PLANE0:
        case GL_CLIP_PLANE1:
        case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3:
        case GL_CLIP_PLANE4:
        case GL_CLIP_PLANE5:
            serverEnabled.clipPlanes &= ~(1 << (cap - GL_CLIP_PLANE0));
            break;
        case GL_COLOR_MATERIAL:
            serverEnabled.colorMaterial = false;
            break;
        case GL_CULL_FACE:
            serverEnabled.cullFace = false;
            GX_SetCullMode(GX_CULL_NONE);
            break;
        case GL_DEPTH_TEST:
            serverEnabled.depthTest = false;
            zEnable = GX_FALSE;
            GX_SetZMode(zEnable, zFunc, zUpdate);
            break;
        case GL_LIGHT0:
        case GL_LIGHT1:
        case GL_LIGHT2:
        case GL_LIGHT3:
        case GL_LIGHT4:
        case GL_LIGHT5:
        case GL_LIGHT6:
        case GL_LIGHT7:
            serverEnabled.lights &= ~(1 << (cap - GL_LIGHT0));
            GX_SetChanCtrl(GX_COLOR1A1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
            break;
        case GL_LIGHTING:
            serverEnabled.lighting = false;
            GX_SetNumChans(1);
            GX_SetNumTevStages(1);
            break;
        case GL_NORMALIZE:
            serverEnabled.normalize = false;
            break;
        case GL_POLYGON_OFFSET_FILL:
            serverEnabled.polygonOffsetFill = false;
            // HACK! manipulate depth test to emulate this
            //GX_SetZMode(zEnable, zFunc, zUpdate);
            break;
        case GL_POINT_SPRITE:
            serverEnabled.pointSprite = false;
            break;
        case GL_STENCIL_TEST:
            serverEnabled.stencilTest = false;
            break;
        case GL_TEXTURE_2D:
            serverEnabled.texture2d = false;
            break;
        case GL_TEXTURE_GEN_S:
            serverEnabled.textureGenS = false;
            break;
        case GL_TEXTURE_GEN_T:
            serverEnabled.textureGenT = false;
            GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
            break;
#ifdef DEBUG
        default:
            fatal_error("glDisable: unknown capability %i\n", cap);
#endif
    }
}
    
void glEnableClientState(GLenum cap)
{
    switch (cap)
    {
        case GL_COLOR_ARRAY:
            clientEnabled.colorArray = true;
            GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
            break;
        case GL_INDEX_ARRAY:
            clientEnabled.indexArray = true;
            break;
        case GL_NORMAL_ARRAY:
            clientEnabled.normalArray = true;
            GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
            break;
        case GL_TEXTURE_COORD_ARRAY:
            clientEnabled.textureCoordArray = true;
            GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
            break;
        case GL_VERTEX_ARRAY:
            clientEnabled.vertexArray = true;
            GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
            break;
#ifdef DEBUG
        default:
            fatal_error("glEnableClientState: invalid capability\n");
#endif
    }
}

struct __gx_regdef
{
	u16 cpSRreg;
	u16 cpCRreg;
	u16 cpCLreg;
	u16 xfFlush;
	u16 xfFlushExp;
	u16 xfFlushSafe;
	u32 gxFifoInited;
	u32 vcdClear;
	u32 VATTable;
	u32 mtxIdxLo;
	u32 mtxIdxHi;
	u32 texCoordManually;
	u32 vcdLo;
	u32 vcdHi;
	u32 vcdNrms;
	u32 dirtyState;
	u32 perf0Mode;
	u32 perf1Mode;
	u32 cpPerfMode;
	u32 VAT0reg[8];
	u32 VAT1reg[8];
	u32 VAT2reg[8];
	u32 texMapSize[8];
	u32 texMapWrap[8];
	u32 sciTLcorner;
	u32 sciBRcorner;
	u32 lpWidth;
	u32 genMode;
	u32 suSsize[8];
	u32 suTsize[8];
	u32 tevTexMap[16];
	u32 tevColorEnv[16];
	u32 tevAlphaEnv[16];
	u32 tevSwapModeTable[8];
	u32 tevRasOrder[11];
	u32 tevTexCoordEnable;
	u32 tevIndMask;
	u32 texCoordGen[8];
	u32 texCoordGen2[8];
	u32 dispCopyCntrl;
	u32 dispCopyDst;
	u32 dispCopyTL;
	u32 dispCopyWH;
	u32 texCopyCntrl;
	u32 texCopyDst;
	u32 texCopyTL;
	u32 texCopyWH;
	u32 peZMode;
	u32 peCMode0;
	u32 peCMode1;
	u32 peCntrl;
	u32 chnAmbColor[2];
	u32 chnMatColor[2];
	u32 chnCntrl[4];
	GXTexRegion texRegion[24];
	GXTlutRegion tlutRegion[20];
	u8 saveDLctx;
	u8 gxFifoUnlinked;
	u8 texCopyZTex;
	u8 _pad;
} __attribute__((packed));
extern u8 __gxregs[];
static struct __gx_regdef *__gx = (struct __gx_regdef*)__gxregs;

void glDisableClientState(GLenum cap)
{
    switch (cap)
    {
        case GL_COLOR_ARRAY:
            clientEnabled.colorArray = false;
            GX_SetVtxDesc(GX_VA_CLR0, GX_NONE);
            break;
        case GL_INDEX_ARRAY:
            clientEnabled.indexArray = false;
            break;
        case GL_NORMAL_ARRAY:
            clientEnabled.normalArray = false;
            GX_SetVtxDesc(GX_VA_NRM, GX_NONE);
            // HACK! GX_SetVtxDesc does not set vcdNrms to zero, so we must do it manually
            __gx->vcdNrms = 0;
            break;
        case GL_TEXTURE_COORD_ARRAY:
            clientEnabled.textureCoordArray = false;
            GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
            break;
        case GL_VERTEX_ARRAY:
            clientEnabled.vertexArray = false;
            GX_SetVtxDesc(GX_VA_POS, GX_NONE);
            break;
#ifdef DEBUG
        default:
            fatal_error("glDisableClientState: invalid capability\n");
#endif
    }
}

//------------------------------------------------------------------------------
// Matrix Stack
//------------------------------------------------------------------------------

void glMatrixMode(GLenum mode)
{
    matrixMode = mode;
    switch (mode)
    {
        case GL_MODELVIEW:
            currMtxStack = &modelviewMtxStack;
            break;
        case GL_PROJECTION:
            currMtxStack = &projMtxStack;
            break;
        case GL_TEXTURE:
            currMtxStack = &textureMtxStack;
            break;
        case GL_COLOR:
            fatal_error("glMatrixMode: color not implemented\n");
            break;
#ifdef DEBUG
        default:
            fatal_error("glMatrixMode: invalid mode\n");
            break;
#endif
    }
}

void glPushMatrix(void)
{
#ifdef DEBUG
    if (currMtxStack->stackPos + 1 >= MTX_STACK_LIMIT)
        fatal_error("glPushMatrix: stack overflow\n");
#endif
    memcpy(currMtxStack->stack[currMtxStack->stackPos + 1],
           currMtxStack->stack[currMtxStack->stackPos], sizeof(Mtx44));
    currMtxStack->stackPos++;
}

static void load_curr_matrix(void)
{
    flush_mem_range(CURR_MATRIX, sizeof(Mtx44));
    switch (matrixMode)
    {
        case GL_MODELVIEW:
        {
            Mtx44 m;
            GX_LoadPosMtxImm(CURR_MATRIX, GX_PNMTX0);
            guMtxInvXpose(CURR_MATRIX, m);
            GX_LoadNrmMtxImm(m, GX_PNMTX0);
            break;
        }
        case GL_PROJECTION:
            GX_LoadProjectionMtx(CURR_MATRIX, GX_PERSPECTIVE);
            break;
        case GL_TEXTURE:
            GX_LoadTexMtxImm(CURR_MATRIX, GX_TEXMTX0, GX_MTX2x4);
            break;
        case GL_COLOR:
            fatal_error("TODO: implement color matrix\n");
            break;
    }
}

static void mult_mtx44(Mtx44 a, Mtx44 b, Mtx44 res)
{
    Mtx44 tmp;
    f32 (*m)[4];
    
    if (res == a || res == b)
        m = tmp;
    else
        m = res;

    m[0][0] = a[0][0]*b[0][0] + a[0][1]*b[1][0] + a[0][2]*b[2][0] + a[0][3]*b[3][0];
    m[0][1] = a[0][0]*b[0][1] + a[0][1]*b[1][1] + a[0][2]*b[2][1] + a[0][3]*b[3][1];
    m[0][2] = a[0][0]*b[0][2] + a[0][1]*b[1][2] + a[0][2]*b[2][2] + a[0][3]*b[3][2];
    m[0][3] = a[0][0]*b[0][3] + a[0][1]*b[1][3] + a[0][2]*b[2][3] + a[0][3]*b[3][3];

    m[1][0] = a[1][0]*b[0][0] + a[1][1]*b[1][0] + a[1][2]*b[2][0] + a[1][3]*b[3][0];
    m[1][1] = a[1][0]*b[0][1] + a[1][1]*b[1][1] + a[1][2]*b[2][1] + a[1][3]*b[3][1];
    m[1][2] = a[1][0]*b[0][2] + a[1][1]*b[1][2] + a[1][2]*b[2][2] + a[1][3]*b[3][2];
    m[1][3] = a[1][0]*b[0][3] + a[1][1]*b[1][3] + a[1][2]*b[2][3] + a[1][3]*b[3][3];

    m[2][0] = a[2][0]*b[0][0] + a[2][1]*b[1][0] + a[2][2]*b[2][0] + a[2][3]*b[3][0];
    m[2][1] = a[2][0]*b[0][1] + a[2][1]*b[1][1] + a[2][2]*b[2][1] + a[2][3]*b[3][1];
    m[2][2] = a[2][0]*b[0][2] + a[2][1]*b[1][2] + a[2][2]*b[2][2] + a[2][3]*b[3][2];
    m[2][3] = a[2][0]*b[0][3] + a[2][1]*b[1][3] + a[2][2]*b[2][3] + a[2][3]*b[3][3];
    
    m[3][0] = a[3][0]*b[0][0] + a[3][1]*b[1][0] + a[3][2]*b[2][0] + a[3][3]*b[3][0];
    m[3][1] = a[3][0]*b[0][1] + a[3][1]*b[1][1] + a[3][2]*b[2][1] + a[3][3]*b[3][1];
    m[3][2] = a[3][0]*b[0][2] + a[3][1]*b[1][2] + a[3][2]*b[2][2] + a[3][3]*b[3][2];
    m[3][3] = a[3][0]*b[0][3] + a[3][1]*b[1][3] + a[3][2]*b[2][3] + a[3][3]*b[3][3];
    
    if (m == tmp)
        memcpy(res, m, sizeof(Mtx44));
}

static void dump_matrix(f32 (*m)[4])
{
    log_printf("matrix:\n");
    for (int i = 0; i < 4; i++)
    {
        log_printf("\t[%.4f, %.4f, %.4f, %.4f]\n",
          m[i][0], m[i][1], m[i][2], m[i][3]);
    }
}

void glPopMatrix(void)
{
    currMtxStack->stackPos--;
#ifdef DEBUG
    if (currMtxStack->stackPos < 0)
        fatal_error("glPopMatrix: stack underflow\n");
#endif
    load_curr_matrix();
}

void glLoadIdentity(void)
{
    Mtx44 m = {
        {1, 0, 0, 0},
        {0, 1, 0, 0},
        {0, 0, 1, 0},
        {0, 0, 0, 1},
    };
    
    memcpy(CURR_MATRIX, m, sizeof(Mtx44));
    load_curr_matrix();
}

void glLoadMatrixf(const GLfloat *m)
{
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
            guMtxRowCol(CURR_MATRIX, r, c) = m[c * 4 + r];
    }
    load_curr_matrix();
}

void glMultMatrixf(const GLfloat *m)
{
    Mtx44 mtx;

    // OpenGL uses column-major matrices, while GX uses row-major matrices,
    // so we need to transpose it.
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
            guMtxRowCol(mtx, r, c) = m[c * 4 + r];
    }
    mult_mtx44(CURR_MATRIX, mtx, CURR_MATRIX);
    load_curr_matrix();
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    Mtx44 m = {
        {1, 0, 0, x},
        {0, 1, 0, y},
        {0, 0, 1, z},
        {0, 0, 0, 1},
    };
    mult_mtx44(CURR_MATRIX, m, CURR_MATRIX);
    load_curr_matrix();
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    Mtx44 m = {
        {x, 0, 0, 0},
        {0, y, 0, 0},
        {0, 0, z, 0},
        {0, 0, 0, 1},
    };
    mult_mtx44(CURR_MATRIX, m, CURR_MATRIX);
    load_curr_matrix();
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    guVector axis = {x, y, z};

    guVecNormalize(&axis);
    angle = angle * 3.14159265359f / 180.0f;
    
    x = axis.x;
    y = axis.y;
    z = axis.z;
    float xsq = x * x;
    float ysq = y * y;
    float zsq = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float s = sinf(angle);
    float c = cosf(angle);
    Mtx44 m;

    guMtxRowCol(m, 0, 0) = xsq*(1-c)+c;
    guMtxRowCol(m, 1, 0) = xy*(1-c)+z*s;
    guMtxRowCol(m, 2, 0) = xz*(1-c)-y*s;
    guMtxRowCol(m, 3, 0) = 0;
    
    guMtxRowCol(m, 0, 1) = xy*(1-c)-z*s;
    guMtxRowCol(m, 1, 1) = ysq*(1-c)+c;
    guMtxRowCol(m, 2, 1) = yz*(1-c)+x*s;
    guMtxRowCol(m, 3, 1) = 0;
    
    guMtxRowCol(m, 0, 2) = xz*(1-c)+y*s;
    guMtxRowCol(m, 1, 2) = yz*(1-c)-x*s;
    guMtxRowCol(m, 2, 2) = zsq*(1-c)+c;
    guMtxRowCol(m, 3, 2) = 0;
    
    guMtxRowCol(m, 0, 3) = 0;
    guMtxRowCol(m, 1, 3) = 0;
    guMtxRowCol(m, 2, 3) = 0;
    guMtxRowCol(m, 3, 3) = 1;
    
    mult_mtx44(CURR_MATRIX, m, CURR_MATRIX);
    load_curr_matrix();
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
  GLdouble nearVal, GLdouble farVal)
{
    float tx = -(right + left) / (right - left);
    float ty = -(top + bottom) / (top - bottom);
    float tz = -(farVal + nearVal) / (farVal - nearVal);
    Mtx44 mtx;

    memset(mtx, 0, sizeof(mtx));
    guMtxRowCol(mtx, 0, 0) = 2.0f / (right - left);
    guMtxRowCol(mtx, 1, 1) = 2.0f / (top - bottom);
    guMtxRowCol(mtx, 2, 2) = -2.0f / (farVal - nearVal);
    guMtxRowCol(mtx, 0, 3) = tx;
    guMtxRowCol(mtx, 1, 3) = ty;
    guMtxRowCol(mtx, 2, 3) = tz;
    guMtxRowCol(mtx, 3, 3) = 1.0f;
    
    guOrtho(mtx, top, bottom, left, right, nearVal, farVal);
    
    mult_mtx44(CURR_MATRIX, mtx, CURR_MATRIX);
    if (matrixMode == GL_PROJECTION)
        GX_LoadProjectionMtx(CURR_MATRIX, GX_ORTHOGRAPHIC);
    else
        load_curr_matrix();
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
  GLdouble nearVal, GLdouble farVal)
{
    guFrustum(CURR_MATRIX, top, bottom, left, right, nearVal, farVal);
    load_curr_matrix();
}

//------------------------------------------------------------------------------

static struct Buffer *get_buffer(GLenum target)
{
    switch (target)
    {
        case GL_ARRAY_BUFFER:         return boundBuffers[0];
        case GL_ELEMENT_ARRAY_BUFFER: return boundBuffers[1];
#ifdef DEBUG
        default:
            fatal_error("bad buffer target %i\n", target);
#endif
    }
    return NULL;
}

static void set_buffer(GLenum target, struct Buffer *buf)
{
    switch (target)
    {
        case GL_ARRAY_BUFFER:         boundBuffers[0] = buf; return;
        case GL_ELEMENT_ARRAY_BUFFER: boundBuffers[1] = buf; return;
#ifdef DEBUG
        default:
            fatal_error("bad buffer target %i\n", target);
#endif
    }
}

void glGenBuffers(GLsizei n, GLuint *buffers)
{
    for (u32 i = 0; i < n; i++)
    {
        struct Buffer *buf = malloc(sizeof(*buf));

        buf->data = NULL;
        buf->size = 0;
        buffers[i] = (GLuint)buf;
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
    struct Buffer *buf;

    for (u32 i = 0; i < n; i++)
    {
        buf = (struct Buffer *)buffers[i];
        if (buf != NULL)
        {
            if (buf->data != NULL)
                free(buf->data);
            free(buf);
        }
    }
}

void glBindBuffer(GLenum target, GLuint buffer)
{
    set_buffer(target, (struct Buffer *)buffer);
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    struct Buffer *buf = get_buffer(target);

    if (buf != NULL)
    {
        if (buf->data != NULL)
            free(buf->data);
        buf->data = malloc(size);
        buf->size = size;
        if (data != NULL)
        {
            memcpy(buf->data, data, size);
            flush_mem_range(buf->data, buf->size);
        }
    }
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
    struct Buffer *buf = get_buffer(target);

#ifdef DEBUG
    if (buf == NULL || buf->data == NULL)
        fatal_error("glBufferSubData: buffer has not been initialized\n");
    if (offset + size > buf->size)
        fatal_error("glBufferSubData: offset + size is too large (%i + %u > %i)\n", offset, size, buf->size);
#endif
    memcpy((u8 *)buf->data + offset, data, size);
    flush_mem_range(buf->data, buf->size);
}

//------------------------------------------------------------------------------
// Drawing Functions
//------------------------------------------------------------------------------

void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha)
{
    GXColor color = {
        .r = red,
        .g = green,
        .b = blue,
        .a = alpha,
    };
    GX_SetTevColor(GX_TEVREG0, color);
    //GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    //GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    switch (size)
    {
        case 2:  size = GX_POS_XY;   break;
        case 3:  size = GX_POS_XYZ;  break;
#ifdef DEBUG
        default:
            fatal_error("glVertexPointer: invalid size\n");
#endif
    }
    type = gl_enum_to_gx(type);
    posDesc.components = size;
    posDesc.format = type;
    posDesc.stride = stride;
    if (get_buffer(GL_ARRAY_BUFFER) != NULL)
        posDesc.pointer = (u8 *)get_buffer(GL_ARRAY_BUFFER)->data + (ptrdiff_t)pointer;
    else
        posDesc.pointer = pointer;
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, posDesc.components, posDesc.format, 0);
    GX_SetArray(GX_VA_POS, (void *)posDesc.pointer, posDesc.stride);
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    switch (size)
    {
        case 3:  size = GX_CLR_RGB;   break;
        case 4:  size = GX_CLR_RGBA;  break;
#ifdef DEBUG
        default:
            fatal_error("glColorPointer: invalid size\n");
#endif
    }
    type = gl_enum_to_gx(type);
    switch (type)
    {
        case GX_U8:
            if (size == GX_CLR_RGB)
                type = GX_RGB8;
            else if (size == GX_CLR_RGBA)
                type = GX_RGBA8;
            else
                fatal_error("glColorPointer: invalid type\n");
            break;
        case GX_F32:
            break;
#ifdef DEBUG
        default:
            fatal_error("glColorPointer: invalid type\n");
#endif
    }
    colorDesc.components = size;
    colorDesc.format = type;
    colorDesc.stride = stride;
    if (get_buffer(GL_ARRAY_BUFFER) != NULL)
        colorDesc.pointer = (u8 *)get_buffer(GL_ARRAY_BUFFER)->data + (ptrdiff_t)pointer;
    else
        colorDesc.pointer = pointer;
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, colorDesc.components, colorDesc.format, 0);
    GX_SetArray(GX_VA_CLR0, (void *)colorDesc.pointer, colorDesc.stride);
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
    switch (size)
    {
        case 1:  size = GX_TEX_S;   break;
        case 2:  size = GX_TEX_ST;  break;
        case 3:  size = GX_TEX_ST;  break;  // ignore the third component
#ifdef DEBUG
        default:
            printf("size %i\n", size);
            fatal_error("glTexCoordPointer: invalid size\n");
#endif
    }
    type = gl_enum_to_gx(type);
    texCoordDesc.components = size;
    texCoordDesc.format = type;
    texCoordDesc.stride = stride;
    if (get_buffer(GL_ARRAY_BUFFER) != NULL)
        texCoordDesc.pointer = (u8 *)get_buffer(GL_ARRAY_BUFFER)->data + (ptrdiff_t)pointer;
    else
        texCoordDesc.pointer = pointer;
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, texCoordDesc.components, texCoordDesc.format, 0);
    GX_SetArray(GX_VA_TEX0, (void *)texCoordDesc.pointer, texCoordDesc.stride);
    
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer)
{
    type = gl_enum_to_gx(type);
    nrmDesc.components = GX_NRM_XYZ;
    nrmDesc.format = type;
    nrmDesc.stride = stride;
    if (get_buffer(GL_ARRAY_BUFFER) != NULL)
        nrmDesc.pointer = (u8 *)get_buffer(GL_ARRAY_BUFFER)->data + (ptrdiff_t)pointer;
    else
        nrmDesc.pointer = pointer;
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, nrmDesc.components, nrmDesc.format, 0);
    GX_SetArray(GX_VA_NRM, (void *)nrmDesc.pointer, nrmDesc.stride);
}

static void setup_drawing(void)
{
    u8 texColorInput;
    u8 texAlphaInput;
    u8 vtxColorInput;
    u8 vtxAlphaInput;

    if (clientEnabled.colorArray)
    {
        vtxColorInput = GX_CC_RASC;
        vtxAlphaInput = GX_CA_RASA;
    }
    else
    {
        vtxColorInput = GX_CC_C0;
        vtxAlphaInput = GX_CA_A0;
    }

    if (clientEnabled.textureCoordArray)
    {
        texColorInput = GX_CC_TEXC;
        texAlphaInput = GX_CA_TEXA;
    }
    else
    {
        texColorInput = GX_CC_ONE;
        // There doesn't seem to be a GX_CA_ONE, so let's just set a register for that.
        GX_SetTevColor(GX_TEVREG1, (GXColor){255, 255, 255, 255});
        texAlphaInput = GX_CA_A1;
    }
    
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, vtxColorInput, texColorInput, GX_CC_ZERO);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, vtxAlphaInput, texAlphaInput, GX_CA_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    mode = gl_enum_to_gx(mode);
    if (serverEnabled.texture2d && boundTexture != NULL)
        GX_LoadTexObj(&boundTexture->texObj, GX_TEXMAP0);
    if (serverEnabled.polygonOffsetFill)
    {
        // Adjust the projection matrix to offset the drawn polygon
        Mtx44 m;
        guMtxApplyTrans(projMtxStack.stack[projMtxStack.stackPos], m, 0, 0, -polyOffsUnits * 0.1);
        GX_LoadProjectionMtx(m, GX_PERSPECTIVE);
    }
    setup_drawing();
    GX_InvVtxCache();
    GX_Begin(mode, GX_VTXFMT0, count);
    for (int i = 0; i < count; i++)
    {
        if (clientEnabled.vertexArray)
            GX_Position1x16(first + i);
        if (clientEnabled.normalArray)
            GX_Normal1x16(first + i);
        if (clientEnabled.colorArray)
            GX_Color1x16(first + i);
        if (clientEnabled.textureCoordArray)
            GX_TexCoord1x16(first + i);
    }
    GX_End();
    
    if (serverEnabled.polygonOffsetFill)
        GX_LoadProjectionMtx(projMtxStack.stack[projMtxStack.stackPos], GX_PERSPECTIVE);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    const u8 *indicesu8;
    const u16 *indicesu16;

    mode = gl_enum_to_gx(mode);
    if (serverEnabled.texture2d && boundTexture != NULL)
        GX_LoadTexObj(&boundTexture->texObj, GX_TEXMAP0);
    if (serverEnabled.polygonOffsetFill)
    {
        // Adjust the projection matrix to offset the drawn polygon
        Mtx44 m;
        guMtxApplyTrans(projMtxStack.stack[projMtxStack.stackPos], m, 0, 0, -polyOffsUnits * 0.1);
        GX_LoadProjectionMtx(m, GX_PERSPECTIVE);
    }
    setup_drawing();
    GX_InvVtxCache();
    if (get_buffer(GL_ELEMENT_ARRAY_BUFFER) != NULL)
        indices = (u8 *)get_buffer(GL_ELEMENT_ARRAY_BUFFER)->data + (u32)indices;
    indicesu8 = indices;
    indicesu16 = indices;
    GX_Begin(mode, GX_VTXFMT0, count);
    for (int i = 0; i < count; i++)
    {
        int index;

        switch (type)
        {
            case GL_UNSIGNED_BYTE:
                index = *(indicesu8++);
                break;
            case GL_UNSIGNED_SHORT:
                index = *(indicesu16++);
                break;
#ifdef DEBUG
            default:
                fatal_error("glDrawElements: bad type parameter\n");
#endif
        }
        if (clientEnabled.vertexArray)
            GX_Position1x16(index);
        if (clientEnabled.normalArray)
            GX_Normal1x16(index);
        if (clientEnabled.colorArray)
            GX_Color1x16(index);
        if (clientEnabled.textureCoordArray)
            GX_TexCoord1x16(index);
    }
    GX_End();
    
    if (serverEnabled.polygonOffsetFill)
        GX_LoadProjectionMtx(projMtxStack.stack[projMtxStack.stackPos], GX_PERSPECTIVE);
}

//------------------------------------------------------------------------------

static void *convert_to_rgb5a3(const u8 *data, u32 width, u32 height)
{
    u32 bufferWidth = round_up(width, 4);
    u32 bufferHeight = round_up(height, 4);
    u16 *buffer = memalign(32, bufferWidth * bufferHeight * sizeof(u16));
    u32 blockCols = width / 4;
    
    memset(buffer, 0, bufferWidth * bufferHeight * sizeof(u16));
    for (u32 x = 0; x < width; x++)
    {
        u32 blockX = x / 4;
        u32 remX = x % 4;

        for (u32 y = 0; y < height; y++)
        {
            u8 r, g, b, a;
            u16 pixel;
            
            if (data[4 * (x + y * width) + 3] == 255)
            {
                r = (data[4 * (x + y * width) + 0] >> 3) & 31;
                g = (data[4 * (x + y * width) + 1] >> 3) & 31;
                b = (data[4 * (x + y * width) + 2] >> 3) & 31;
                pixel = (1 << 15) | (r << 10) | (g << 5) | b;
            }
            else
            {
                r = (data[4 * (x + y * width) + 0] >> 4) & 15;
                g = (data[4 * (x + y * width) + 1] >> 4) & 15;
                b = (data[4 * (x + y * width) + 2] >> 4) & 15;
                a = (data[4 * (x + y * width) + 3] >> 5) & 7;
                pixel = (a << 12) | (r << 8) | (g << 4) | b;
            }
            
            u32 blockY = y / 4;
            u32 remY = y % 4;
            u32 index = 16 * (blockX + blockY * blockCols) + (remY * 4 + remX);
            buffer[index] = pixel;
        }
    }
    flush_mem_range(buffer, bufferWidth * bufferHeight * sizeof(u16));
    return buffer;
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    for (u32 i = 0; i < n; i++)
    {
        struct Texture *tex = malloc(sizeof(*tex));

        tex->imgBuffer = NULL;
        tex->initialized = false;
        tex->magFilter = GX_LINEAR;
        tex->minFilter = GX_LINEAR;
        textures[i] = (GLuint)tex;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    struct Texture *tex;

    for (u32 i = 0; i < n; i++)
    {
        tex = (struct Texture *)textures[i];
        if (tex != NULL)
        {
            if (tex->imgBuffer != NULL)
                free(tex->imgBuffer);
            free(tex);
        }
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
  GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
  const GLvoid *data)
{
    struct Texture *tex = boundTexture;

#ifdef DEBUG
    if (tex == NULL)
        fatal_error("glTexImage2D: no texture is bound\n");
    if (type != GL_UNSIGNED_BYTE)
        fatal_error("glTexImage2D: unsupported type\n");
#endif
    switch (internalformat)
    {
        case GL_ALPHA:
        {
            u8 *temp = malloc(4 * width * height);
            const u8 *data8 = data;
            
            for (int x = 0; x < width; x++)
            {
                for (int y = 0; y < height; y++)
                {
                    u8 alpha = data8[x + y * width];

                    temp[4 * (x + y * width) + 0] = 0;
                    temp[4 * (x + y * width) + 1] = 0;
                    temp[4 * (x + y * width) + 2] = 0;
                    temp[4 * (x + y * width) + 3] = alpha;
                }
            }
            tex->imgBuffer = convert_to_rgb5a3(temp, width, height);
            free(temp);
            break;
        }
        case GL_LUMINANCE:
        {
            u8 *temp = malloc(4 * width * height);
            const u8 *data8 = data;
            
            for (int x = 0; x < width; x++)
            {
                for (int y = 0; y < height; y++)
                {
                    u8 lum = data8[x + y * width];
                    
                    temp[4 * (x + y * width) + 0] = lum;
                    temp[4 * (x + y * width) + 1] = lum;
                    temp[4 * (x + y * width) + 2] = lum;
                    temp[4 * (x + y * width) + 3] = 0xFF;
                }
            }
            tex->imgBuffer = convert_to_rgb5a3(temp, width, height);
            free(temp);
            break;
        }
        case GL_LUMINANCE_ALPHA:
        {
            u8 *temp = malloc(4 * width * height);
            const u8 *data8 = data;
            
            for (int x = 0; x < width; x++)
            {
                for (int y = 0; y < height; y++)
                {
                    u8 lum = data8[2 * (x + y * width) + 0];
                    u8 alpha = data8[2 * (x + y * width) + 1];

                    temp[4 * (x + y * width) + 0] = lum;
                    temp[4 * (x + y * width) + 1] = lum;
                    temp[4 * (x + y * width) + 2] = lum;
                    temp[4 * (x + y * width) + 3] = alpha;
                }
            }
            tex->imgBuffer = convert_to_rgb5a3(temp, width, height);
            free(temp);
            break;
        }
        case GL_RGB:
        {
            u8 *temp = malloc(4 * width * height);
            const u8 *data8 = data;
            
            for (int x = 0; x < width; x++)
            {
                for (int y = 0; y < height; y++)
                {
                    temp[4 * (x + y * width) + 0] = data8[3 * (x + y * width) + 0];
                    temp[4 * (x + y * width) + 1] = data8[3 * (x + y * width) + 1];
                    temp[4 * (x + y * width) + 2] = data8[3 * (x + y * width) + 2];
                    temp[4 * (x + y * width) + 3] = 0xFF;
                }
            }
            tex->imgBuffer = convert_to_rgb5a3(temp, width, height);
            free(temp);
            break;
        }
        case GL_RGBA:
            tex->imgBuffer = convert_to_rgb5a3(data, width, height);
            break;
        default:
            fatal_error("glTexImage2D: unknown format %i\n", internalformat);
            return;
    }
#ifdef DEBUG
    if (tex->imgBuffer == NULL)
        fatal_error("failed to convert texture");
#endif
    GX_InitTexObj(&tex->texObj, tex->imgBuffer, width, height, GX_TF_RGB5A3,
                  GX_CLAMP, GX_CLAMP, GX_FALSE);
    tex->initialized = true;
    GX_InitTexObjFilterMode(&tex->texObj, tex->minFilter, tex->magFilter);
    GX_InvalidateTexAll();
}

void glBindTexture(GLenum target, GLuint texture)
{
    struct Texture *tex = (struct Texture *)texture;

#ifdef DEBUG
    if (target != GL_TEXTURE_2D)
        fatal_error("glBindTexture: invalid texture type\n");
#endif
    boundTexture = tex;
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    GXTexObj *texObj;
    void *imagePtr;
    u16 width;
    u16 height;
    u8 format;
    u8 wrapS;
    u8 wrapT;
    u8 mipmap;

#ifdef DEBUG
    if (boundTexture == NULL)
        fatal_error("glTexParameteri: no texture is bound\n");
#endif
    texObj = &boundTexture->texObj;
    GX_GetTexObjAll(texObj, &imagePtr, &width, &height, &format,
      &wrapS, &wrapT, &mipmap);
    switch (pname)
    {
        case GL_TEXTURE_MAG_FILTER:
            boundTexture->magFilter = gl_enum_to_gx(param);
            GX_InitTexObjFilterMode(texObj, boundTexture->minFilter, boundTexture->magFilter);
            break;
        case GL_TEXTURE_MIN_FILTER:
            boundTexture->minFilter = gl_enum_to_gx(param);
            GX_InitTexObjFilterMode(texObj, boundTexture->minFilter, boundTexture->magFilter);
            break;
        case GL_TEXTURE_WRAP_S:
            wrapS = gl_enum_to_gx(param);
            GX_InitTexObjWrapMode(texObj, wrapS, wrapT);
            break;
        case GL_TEXTURE_WRAP_T:
            wrapT = gl_enum_to_gx(param);
            GX_InitTexObjWrapMode(texObj, wrapS, wrapT);
            break;
        
#ifdef DEBUG
        default:
            printf("pname = %X\n", pname);
            fatal_error("glTexParameteri: unknown pname %i\n", pname);
#endif
    }
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params)
{
    // TODO: implement
    switch (pname)
    {
        case GL_AMBIENT:
            GX_SetChanAmbColor(GX_COLOR1A1, (GXColor){params[0] * 255, params[1] * 255, params[2] * 255, params[3] * 255});
            break;
        case GL_DIFFUSE:
            GX_SetChanMatColor(GX_COLOR1A1, (GXColor){params[0] * 255, params[1] * 255, params[2] * 255, params[3] * 255});
            break;
    }
}

int blend_factor(int f)
{
    switch (f)
    {
        case GL_ZERO:                   return GX_BL_ZERO;
        case GL_ONE:                    return GX_BL_ONE;
        case GL_SRC_COLOR:              return GX_BL_SRCCLR;
        case GL_ONE_MINUS_SRC_COLOR:    return GX_BL_INVSRCCLR;
        case GL_DST_COLOR:              return GX_BL_DSTCLR;
        case GL_ONE_MINUS_DST_COLOR:    return GX_BL_INVDSTCLR;
        case GL_SRC_ALPHA:              return GX_BL_SRCALPHA;
        case GL_ONE_MINUS_SRC_ALPHA:    return GX_BL_INVSRCALPHA;
        case GL_DST_ALPHA:              return GX_BL_DSTALPHA;
        case GL_ONE_MINUS_DST_ALPHA:    return GX_BL_INVDSTALPHA;
        default:
            fatal_error("unknown blend factor %i\n", f);
    }
    return 0;
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    // TODO: implement
    sfactor = blend_factor(sfactor);
    dfactor = blend_factor(dfactor);
    GX_SetBlendMode(GX_BM_BLEND, sfactor, dfactor, 0);
}

void glTexGeni(GLenum coord, GLenum pname, GLint param)
{
    // TODO: implement
    if (coord == GL_T && pname == GL_TEXTURE_GEN_MODE && param == GL_SPHERE_MAP)
    {
        // Ugh. Help! I can't get this right.
        Mtx m;
        guMtxInvXpose(modelviewMtxStack.stack[modelviewMtxStack.stackPos], m);
        //guMtxApplyScale(m, m, 0.5, -0.5, 0);
        //guMtxApplyTrans(m, m, 0.5, 0.5, 1);
        guMtxApplyTrans(m, m, 0.5, -0.5, 0);
        guMtxApplyScale(m, m, 0.5, 0.5, 1);
        
        GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX2x4);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_NRM, GX_TEXMTX0);
    }
}

void glPointParameterfv(GLenum pname, const GLfloat *params)
{
    // TODO: implement
}

void glTexEnvi(GLenum target, GLenum pname, GLfloat param)
{
    // TODO: implement
}

void glAlphaFunc(GLenum func, GLclampf ref)
{
    // TODO: implement
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    GXColor color = {
        .r = red * 255,
        .g = green * 255,
        .b = blue * 255,
        .a = alpha * 255,
    };
    GX_SetTevColor(GX_TEVREG0, color);
}

void glDepthMask(GLboolean flag)
{
    zUpdate = flag;
    GX_SetZMode(zEnable, zFunc, zUpdate);
}

void glFrontFace(GLenum mode)
{
    // TODO: implement
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    // TODO: implement
}

void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass)
{
    // TODO: implement
}

void glPolygonOffset(GLfloat factor, GLfloat units)
{
    // TODO: implement
    polyOffsFactor = factor;
    polyOffsUnits = units;
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
    // TODO: implement
}

void glClear(GLbitfield mask)
{
    // TODO: implement
}

void glClipPlane(GLenum plane, const GLdouble *equation)
{
    // TODO: implement
}

void glActiveTexture(GLenum texture)
{
    // TODO: implement
}

void glClientActiveTexture(GLenum texture)
{
    // TODO: implement
}

void glPointParameterf(GLenum pname, GLfloat param)
{
    // TODO: implement
}

void glPixelStorei(GLenum pname, GLint param)
{
    // TODO: implement
}

void glDepthFunc(GLenum func)
{
    zFunc = gl_enum_to_gx(func);
    GX_SetZMode(zEnable, zFunc, zUpdate);
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    // TODO: implement
    /*
    GX_SetViewport(x, y, width, height, 0, 1);
    GX_SetScissor(x, y, width, height);
    GX_SetDispCopyYScale(GX_GetYScaleFactor(videoMode->efbHeight, videoMode->xfbHeight));
    */
}

void glLightModeli(GLenum pname, GLint param)
{
    // TODO: implement
}

void glLightModelf(GLenum pname, GLfloat param)
{
    // TODO: implement
}

void glLightModelfv(GLenum pname, const GLfloat *params)
{
    switch (pname)
    {
        // Only one we need to support
        case GL_LIGHT_MODEL_AMBIENT:
            GX_SetChanAmbColor(GX_COLOR1A1, (GXColor){params[0] * 255, params[1] * 255, params[2] * 255, params[3] * 255});
            break;
    }
}

static void mult_mtx44_vec4(Mtx44 m, float *in, float *out)
{
    float temp[4];
    
    temp[0] = m[0][0]*in[0] + m[0][1]*in[1] + m[0][2]*in[2] + m[0][3]*in[3];
    temp[1] = m[1][0]*in[0] + m[1][1]*in[1] + m[1][2]*in[2] + m[1][3]*in[3];
    temp[2] = m[2][0]*in[0] + m[2][1]*in[1] + m[2][2]*in[2] + m[2][3]*in[3];
    temp[3] = m[3][0]*in[0] + m[3][1]*in[1] + m[3][2]*in[2] + m[3][3]*in[3];
    memcpy(out, temp, 4 * sizeof(float));
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *params)
{
    // TODO: implement
    int lightNum = light - GL_LIGHT0;
    switch (pname)
    {
        case GL_POSITION:
        {
            /*
            guVector lightPos = {params[0], params[1], params[2]};
            guVecMultiply(modelviewMtxStack.stack[modelviewMtxStack.stackPos], &lightPos, &lightPos);
            GX_InitLightPos(&lightObj[lightNum], lightPos.x, lightPos.y, lightPos.z);
            */
            f32 lightPos[] = {params[0], params[1], params[2], params[3]};
            Mtx44 m;
            //guMtxInverse(modelviewMtxStack.stack[modelviewMtxStack.stackPos], m);
            mult_mtx44_vec4(modelviewMtxStack.stack[modelviewMtxStack.stackPos], lightPos, lightPos);
            //dump_matrix(modelviewMtxStack.stack[modelviewMtxStack.stackPos]);
            GX_InitLightPos(&lightObj[lightNum], lightPos[0], lightPos[1], lightPos[2]);
            break;
        }
        case GL_DIFFUSE:
            GX_InitLightColor(&lightObj[lightNum], (GXColor){params[0] * 255, params[1] * 255, params[2] * 255});
            break;
        case GL_AMBIENT:
            break;
        case GL_SPECULAR:
            break;
    }
    GX_LoadLightObj(&lightObj[lightNum], (1 << lightNum));
}

void glPointSize(GLfloat size)
{
    // TODO: implement
    GX_SetPointSize(128, GX_TO_ZERO);
}

void glCullFace(GLenum mode)
{
    cullMode = gl_enum_to_gx(mode);
    GX_SetCullMode(cullMode);
}

void glGetIntegerv(GLenum pname, GLint *data)
{
    switch (pname)
    {
        case GL_MAX_TEXTURE_SIZE:
            *data = 1024;
            break;
        case GL_MAX_TEXTURE_UNITS:
            *data = 8;
            break;
#ifdef DEBUG
        default:
            fatal_error("glGetIntegerv: unknown pname %i\n", pname);
#endif
    }
}

const GLubyte *glGetString(GLenum name)
{
    switch (name)
    {
        case GL_VENDOR:
            return (GLubyte *)"WiiGL";
        case GL_RENDERER:
            return (GLubyte *)"WiiGL OpenGL wrapper for Neverball on Nintendo Wii";
        case GL_VERSION:
            return (GLubyte *)"0.1";
        case GL_EXTENSIONS:
            return (GLubyte *)"";
#ifdef DEBUG
        default:
            fatal_error("glGetString: unknown name %i\n", name);
#endif
    }
    return NULL;
}

void glPolygonMode(GLenum face, GLenum mode)
{
    // TODO: implement
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
  GLenum format, GLenum type, GLvoid *data)
{
    // TODO: implement
}
