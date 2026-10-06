#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <math.h>

/* 
 * Boilerplate hardware/module setup 
 */
PSP_MODULE_INFO("Liwa Sandbox", 0, 1, 1);
// THREAD_ATTR_VFPU is required when using -lpspvfpu and math functions
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU); 

#define printf pspDebugScreenPrintf

/* 
 * Required PSP callbacks so the emulator/hardware can exit cleanly 
 */
static int exitRequest = 0;
int exit_callback(int arg1, int arg2, void *common) {
    exitRequest = 1;
    return 0;
}
int CallbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
int SetupCallbacks(void) {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
    return thid;
}

/* 
 * Graphics constraints and variables 
 */
#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
static unsigned int __attribute__((aligned(16))) list[262144];

// Colored vertex structure for 3D rendering
struct Vertex {
    unsigned int color;
    float x, y, z;
};

// A massive 3D plane for the "Liwa Sandbox" desert floor
// ABGR color format is used by PSP: 0xFF8CB4D2 is a sand color
struct Vertex __attribute__((aligned(16))) sand_vertices[4] = {
    {0xFF8CB4D2, -500.0f, 0.0f, -500.0f}, 
    {0xFF8CB4D2,  500.0f, 0.0f, -500.0f},
    {0xFF8CB4D2, -500.0f, 0.0f,  500.0f},
    {0xFF8CB4D2,  500.0f, 0.0f,  500.0f},
};

int main(void) {
    SetupCallbacks();
    pspDebugScreenInit();

    // Initialize the Graphics Utility (GU)
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void*)(BUF_WIDTH*SCR_HEIGHT*4), BUF_WIDTH);
    sceGuDepthBuffer((void*)(BUF_WIDTH*SCR_HEIGHT*4*2), BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH/2), 2048 - (SCR_HEIGHT/2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_CULL_FACE);
    sceGuCullMode(GU_CCW);
    sceGuFrontFace(GU_CW);
    sceGuClearColor(0xFFE0B080); // Light blue desert sky
    sceGuClearDepth(0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuDisplay(GU_TRUE);

    // Setup controls (ensure analog stick is mapped)
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    // Player/Camera variables
    float cam_x = 0.0f, cam_y = 2.0f, cam_z = 0.0f;
    float rot_y = 0.0f;
    SceCtrlData pad;

    /* 
     * Main Game Loop 
     */
    while(!exitRequest) {
        sceCtrlReadBufferPositive(&pad, 1);

        // Movement logic via analog stick
        float move_x = (pad.Lx - 128) / 128.0f;
        float move_z = (pad.Ly - 128) / 128.0f;
        
        // Analog deadzone to prevent drift on real hardware/emulators
        if (fabs(move_x) < 0.2f) move_x = 0;
        if (fabs(move_z) < 0.2f) move_z = 0;

        // Rotation via D-Pad
        if (pad.Buttons & PSP_CTRL_LEFT)  rot_y -= 0.05f;
        if (pad.Buttons & PSP_CTRL_RIGHT) rot_y += 0.05f;

        // Calculate forward/backward/strafing based on camera rotation
        cam_x += move_x * cosf(rot_y) * 0.3f + move_z * sinf(rot_y) * 0.3f;
        cam_z += -move_x * sinf(rot_y) * 0.3f + move_z * cosf(rot_y) * 0.3f;

        sceGuStart(GU_DIRECT, list);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        // Set up 3D Perspective Projection
        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(75.0f, 16.0f/9.0f, 0.5f, 1000.0f);

        // Set up View Matrix (Camera LookAt)
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        
        ScePspFVector3 pos = {cam_x, cam_y, cam_z};
        ScePspFVector3 lookAt = {
            cam_x + sinf(rot_y),
            cam_y,
            cam_z + cosf(rot_y)
        };
        ScePspFVector3 up = {0.0f, 1.0f, 0.0f};
        sceGumLookAt(&pos, &lookAt, &up);

        // Set up Model Matrix (World position)
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();

        // Render the sandbox floor
        sceGumDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, 0, sand_vertices);

        sceGuFinish();
        sceGuSync(0, 0);
        
        // Print HUD text
        pspDebugScreenSetXY(0, 0);
        printf("Liwa Sandbox\n");
        printf("X: %.2f  Z: %.2f\n", cam_x, cam_z);
        printf("Analog: Move | D-Pad L/R: Rotate Camera");

        // Wait for screen refresh to prevent screen tearing
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    sceGuTerm();
    sceKernelExitGame();
    return 0;
}