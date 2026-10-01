#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <math.h>
#include <string.h>

PSP_MODULE_INFO("GTA_Liwa", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define printf pspDebugScreenPrintf
#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

static int exitRequest = 0;
int exit_callback(int arg1, int arg2, void *common) { exitRequest = 1; return 0; }
int CallbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid); sceKernelSleepThreadCB(); return 0;
}
int SetupCallbacks(void) {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0); return thid;
}

// --- 3D ENGINE & MATH ---
static unsigned int __attribute__((aligned(16))) list[262144];
struct Vertex { unsigned int color; float x, y, z; };

void DrawRotatedBox(float x, float y, float z, float sx, float sy, float sz, float rotY, unsigned int color) {
    struct Vertex* verts = (struct Vertex*)sceGuGetMemory(36 * sizeof(struct Vertex));
    float p[8][3] = {{-0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f}, { 0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f},
                     {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}};
    int idx[36] = {0,1,2,2,3,0, 1,5,6,6,2,1, 5,4,7,7,6,5, 4,0,3,3,7,4, 3,2,6,6,7,3, 4,5,1,1,0,4};

    for(int i = 0; i < 36; i++) {
        verts[i].color = color;
        verts[i].x = p[idx[i]][0]; verts[i].y = p[idx[i]][1]; verts[i].z = p[idx[i]][2];
    }
    sceGumPushMatrix();
    ScePspFVector3 pos = {x, y, z}; ScePspFVector3 scale = {sx, sy, sz};
    sceGumTranslate(&pos); sceGumRotateY(rotY); sceGumScale(&scale);
    sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D, 36, 0, verts);
    sceGumPopMatrix();
}

int CheckCollision(float x1, float z1, float w1, float d1, float x2, float z2, float w2, float d2) {
    return (fabsf(x1 - x2) * 2.0f < (w1 + w2)) && (fabsf(z1 - z2) * 2.0f < (d1 + d2));
}

// --- GAME DATA STRUCTURES ---
enum { STATE_LOADING, STATE_FREE_ROAM, STATE_INSIDE };

typedef struct { float x, z, rot; int active; char name[32]; unsigned int color; } Vehicle;
Vehicle cars[2] = {
    { 10.0f, 15.0f, 0.0f, 1, "Nissan Patrol VTC", 0xFFFFFFFF },
    {-15.0f, 25.0f, 0.0f, 1, "Toyota Land Cruiser", 0xFF222222 }
};

typedef struct { float x, z, w, d; char name[32]; char action[64]; int price; unsigned int color; } Building;
Building map[6] = {
    {0, 0, 4.0f, 4.0f, "Liwa Safehouse", "Save Game", 0, 0xFF00FF00}, // 0: Save
    {20, 30, 4.0f, 4.0f, "Ammu-Nation", "Buy AK-47", 1000, 0xFF0000FF}, // 1: Guns
    {-20, 40, 4.0f, 4.0f, "Desert Threads", "Buy New Clothes", 200, 0xFF00FFFF}, // 2: Clothes
    {40, 10, 4.0f, 4.0f, "Sand Dune Motors", "Look outside for cars", 0, 0xFFFF00FF}, // 3: Cars
    {0, 50, 4.0f, 4.0f, "Central Bank", "Rob Bank ($5000)", 0, 0xFF888888}, // 4: Robbery
    {30, -20, 4.0f, 4.0f, "Liwa Hospital", "Heal to 100HP", 100, 0xFFFFFFFF} // 5: Heal
};

// Save Data Layout
typedef struct { float x, z; int money, health, gun; } SaveData;
float p_x = 0.0f, p_z = -10.0f, p_rot = 0.0f;
int money = 500, player_health = 100, has_gun = 0;

void SaveGame() {
    SceUID fd = sceIoOpen("ms0:/gta_liwa.dat", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        SaveData sd = {p_x, p_z, money, player_health, has_gun};
        sceIoWrite(fd, &sd, sizeof(SaveData)); sceIoClose(fd);
    }
}
void LoadGame() {
    SceUID fd = sceIoOpen("ms0:/gta_liwa.dat", PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        SaveData sd;
        sceIoRead(fd, &sd, sizeof(SaveData)); sceIoClose(fd);
        p_x = sd.x; p_z = sd.z; money = sd.money; player_health = sd.health; has_gun = sd.gun;
    }
}

int main(void) {
    SetupCallbacks(); pspDebugScreenInit(); sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void*)(BUF_WIDTH*SCR_HEIGHT*4), BUF_WIDTH);
    sceGuDepthBuffer((void*)(BUF_WIDTH*SCR_HEIGHT*4*2), BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH/2), 2048 - (SCR_HEIGHT/2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0); sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST); sceGuEnable(GU_DEPTH_TEST); sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_CULL_FACE); sceGuClearColor(0xFFFFCCAA); sceGuClearDepth(0);
    
    // Setup Hardware Blending for the Fade Effect
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish(); sceGuSync(0, 0); sceGuDisplay(GU_TRUE);

    sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    int state = STATE_LOADING;
    float speed = 0.0f;
    int wanted_stars = 0, is_driving = 0, active_car = -1, inside_bldg_id = -1;
    float p_width = 0.8f, p_depth = 0.8f;

    // AI Variables
    float police_x = 5.0f, police_z = -20.0f, police_rot = 0.0f;
    int cop_state = 0; // 0=IDLE, 1=CHASE, 2=SEARCH
    int los_timer = 0, search_timer = 0;
    
    int frame_timer = 0;
    float fade_alpha = 255.0f;

    struct Vertex __attribute__((aligned(16))) floor[4] = {
        {0xFF44AADD, -500.0f, 0.0f, -500.0f}, {0xFF44AADD,  500.0f, 0.0f, -500.0f},
        {0xFF44AADD, -500.0f, 0.0f,  500.0f}, {0xFF44AADD,  500.0f, 0.0f,  500.0f},
    };

    SceCtrlData pad, old_pad; sceCtrlReadBufferPositive(&old_pad, 1);

    while(!exitRequest) {
        sceCtrlReadBufferPositive(&pad, 1);
        int pressed = pad.Buttons & ~old_pad.Buttons;
        frame_timer++;

        if (state == STATE_LOADING) {
            pspDebugScreenSetXY(0, 0);
            printf("\n\n\n\n\n             LOADING SAVE DATA...");
            if (frame_timer > 60) { LoadGame(); state = STATE_FREE_ROAM; frame_timer = 0; }
            
        } else if (state == STATE_INSIDE) {
            pspDebugScreenSetXY(0, 0);
            printf("LOCATION: %s\nMONEY: $%d | HP: %d | WANTED: %d\n", map[inside_bldg_id].name, money, player_health, wanted_stars);
            printf("[CROSS] %s | [CIRCLE] Exit\n", map[inside_bldg_id].action);

            if (pressed & PSP_CTRL_CROSS) {
                if (inside_bldg_id == 0) { SaveGame(); wanted_stars = 0; } // Safehouse
                if (inside_bldg_id == 1 && money >= map[1].price) { money -= map[1].price; has_gun = 1; }
                if (inside_bldg_id == 2 && money >= map[2].price) { money -= map[2].price; wanted_stars = 0; }
                if (inside_bldg_id == 4) { money += 5000; wanted_stars = 5; state = STATE_FREE_ROAM; } 
                if (inside_bldg_id == 5 && money >= map[5].price) { money -= map[5].price; player_health = 100; } // Hospital
            }
            if (pressed & PSP_CTRL_CIRCLE) state = STATE_FREE_ROAM;

        } else if (state == STATE_FREE_ROAM) {
            // Player Death
            if (player_health <= 0) {
                player_health = 100; money = (money > 500) ? money - 500 : 0; 
                wanted_stars = 0; p_x = map[5].x; p_z = map[5].z + 10.0f; is_driving = 0; // Spawn at hospital
            }

            float move_x = (pad.Lx - 128) / 128.0f; float move_y = (pad.Ly - 128) / 128.0f;
            if (fabs(move_x) < 0.2f) move_x = 0; if (fabs(move_y) < 0.2f) move_y = 0;
            if (pad.Buttons & PSP_CTRL_LTRIGGER) p_rot -= 0.05f; if (pad.Buttons & PSP_CTRL_RTRIGGER) p_rot += 0.05f;

            float next_x = p_x, next_z = p_z;

            if (is_driving) {
                p_width = 2.0f; p_depth = 4.0f;
                speed -= move_y * 0.05f; p_rot += move_x * 0.05f; speed *= 0.95f; 
                next_x += sinf(p_rot) * speed; next_z += cosf(p_rot) * speed;
                if (pressed & PSP_CTRL_TRIANGLE) { is_driving = 0; active_car = -1; speed = 0; p_width = 0.8f; p_depth = 0.8f; }
            } else {
                next_x += move_x * cosf(p_rot) * 0.2f + move_y * sinf(p_rot) * 0.2f;
                next_z += -move_x * sinf(p_rot) * 0.2f + move_y * cosf(p_rot) * 0.2f;

                if (pressed & PSP_CTRL_SQUARE && has_gun) wanted_stars = (wanted_stars < 5) ? wanted_stars + 1 : 5;
                if (pressed & PSP_CTRL_TRIANGLE) {
                    for(int i=0; i<2; i++) {
                        if (sqrtf(powf(p_x - cars[i].x, 2) + powf(p_z - cars[i].z, 2)) < 3.0f) { is_driving = 1; active_car = i; break; }
                    }
                    if (!is_driving) {
                        for(int i=0; i<6; i++) {
                            if (sqrtf(powf(p_x - map[i].x, 2) + powf(p_z - map[i].z, 2)) < 4.0f) { state = STATE_INSIDE; inside_bldg_id = i; break; }
                        }
                    }
                }
            }

            int collision = 0;
            for (int i=0; i<6; i++) {
                if (CheckCollision(next_x, next_z, p_width, p_depth, map[i].x, map[i].z, map[i].w, map[i].d)) { collision = 1; speed *= -0.5f; break; }
            }
            if (!collision) {
                p_x = next_x; p_z = next_z;
                if (is_driving) { cars[active_car].x = p_x; cars[active_car].z = p_z; cars[active_car].rot = p_rot; }
            }

            // POLICE AI STATE MACHINE
            if (wanted_stars > 0) {
                float dx = p_x - police_x, dz = p_z - police_z;
                float dist = sqrtf(dx*dx + dz*dz);
                
                int in_sight = (dist < 20.0f); // Line of sight distance

                if (in_sight) {
                    cop_state = 1; los_timer = 0; search_timer = 0; // Chase Mode
                } else {
                    if (cop_state == 1) { // Losing sight
                        los_timer++;
                        if (los_timer > 420) { cop_state = 2; search_timer = 0; } // 7 Secs at 60fps -> Search Mode
                    } else if (cop_state == 2) {
                        search_timer++;
                        if (search_timer > 600) { wanted_stars = 0; cop_state = 0; los_timer = 0; } // 10 Secs at 60fps -> Lost
                    }
                }

                if (cop_state == 1) { // Aggressive Chase
                    police_rot = atan2f(dx, dz);
                    float p_speed = 0.15f + (wanted_stars * 0.05f);
                    police_x += sinf(police_rot) * p_speed; police_z += cosf(police_rot) * p_speed;
                    
                    if (dist < 3.0f && frame_timer % 30 == 0) player_health -= 15; // Cop deals damage
                } else if (cop_state == 2) { // Searching area
                    police_rot += (frame_timer % 60 == 0) ? 1.5f : 0.0f; // Look around
                    police_x += sinf(police_rot) * 0.1f; police_z += cosf(police_rot) * 0.1f;
                }
            }

            // 3D RENDERING
            sceGuStart(GU_DIRECT, list);
            sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
            sceGumMatrixMode(GU_PROJECTION); sceGumLoadIdentity(); sceGumPerspective(60.0f, 16.0f/9.0f, 0.5f, 1000.0f);
            sceGumMatrixMode(GU_VIEW); sceGumLoadIdentity();
            
            float cam_dist = is_driving ? 7.0f : 4.0f;
            ScePspFVector3 cam_pos = {p_x - sinf(p_rot)*cam_dist, 3.5f, p_z - cosf(p_rot)*cam_dist};
            ScePspFVector3 cam_look = {p_x, 1.0f, p_z}; ScePspFVector3 up = {0.0f, 1.0f, 0.0f};
            sceGumLookAt(&cam_pos, &cam_look, &up);

            sceGumMatrixMode(GU_MODEL); sceGumLoadIdentity();
            sceGumDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D, 4, 0, floor);

            for (int i=0; i<6; i++) DrawRotatedBox(map[i].x, 2.0f, map[i].z, map[i].w, 4.0f, map[i].d, 0.0f, map[i].color); 
            for (int i=0; i<2; i++) {
                if (i != active_car) DrawRotatedBox(cars[i].x, 0.8f, cars[i].z, 2.0f, 1.5f, 4.0f, cars[i].rot, cars[i].color);
            }
            if (!is_driving) DrawRotatedBox(p_x, 1.0f, p_z, p_width, 2.0f, p_depth, p_rot, 0xFF00A0FF);
            
            if (wanted_stars > 0) {
                unsigned int cop_color = (frame_timer % 30 < 15) ? 0xFFFF0000 : 0xFF0000FF; 
                DrawRotatedBox(police_x, 0.8f, police_z, 2.0f, 1.5f, 4.0f, police_rot, cop_color);
            }

            // Screen Fade-in effect on load
            if (fade_alpha > 0) {
                DrawRotatedBox(p_x, 1.0f, p_z, 100.0f, 100.0f, 100.0f, 0.0f, ((int)fade_alpha << 24) | 0x000000); // Black box covering screen
                fade_alpha -= 5.0f;
            }

            sceGuFinish(); sceGuSync(0, 0);

            // HUD
            pspDebugScreenSetXY(0, 0);
            printf("HP: %d | $: %d | WANTED: ", player_health, money);
            if (cop_state == 2) printf("SEARCHING...");
            else for(int s=0; s<5; s++) printf(s < wanted_stars ? "*" : " ");
        }

        old_pad = pad;
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        if(state != STATE_FREE_ROAM) sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT); 
    }

    sceGuTerm(); sceKernelExitGame();
    return 0;
}