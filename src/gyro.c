/*
 * Switch gyro aiming for AvP.
 *
 * The motion is fed in as extra "mouse counts" (see CheckForWindowsMessages in
 * main.c), so it goes through the same MouseVelX/Y path and the same sensitivity
 * options as a real mouse.
 *
 * - Reads every fresh six-axis sample since the last frame and averages them,
 *   instead of one sample per frame (which smears the motion).
 * - Yaw is the angular velocity projected on the gravity direction measured by
 *   the accelerometer, so it is rotation about true vertical regardless of how
 *   the console is tilted (handheld and Pro Controller feel the same).
 * - Pitch is device X, "tilt" (device Y) is added to the turn.
 * - Settings are read from sdmc:/switch/avp/gyro.cfg ("key value" per line) so
 *   they can be tuned without a rebuild. Not stored in any game config file.
 */
#include <switch.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAXSAMPLES 17

static struct {
	int   enabled;
	float yaw, tilt, wheel, pitch;  /* gains, 1.0 = 1:1 with the calibrated base; yaw=gravity projection, tilt=device Y, wheel=device Z */
	int   drift;             /* HidGyroscopeZeroDriftMode: 0 Loose, 1 Standard, 2 Tight */
	float cpd;               /* mouse counts per degree at gain 1.0 */
	float deadzone;          /* deg/s */
	int   invYaw, invPitch;
	int   verbose;
} cfg = { 1, 0.0f, 1.067f, 1.333f, 1.067f, 0, 5.9f, 0.0f, 0, 1, 0 };

static HidSixAxisSensorHandle handles[2];
static int      haveHandle;
static HidNpadIdType curId;
static HidNpadStyleTag curStyle;
static u64      lastSample;
static int      frames;
static double   remX, remY;   /* sub-count remainders so slow motion isn't lost */
static double   accumYaw, accumPitch;
static int      logTimer;

static void load_cfg(void)
{
	FILE *f = fopen("sdmc:/switch/avp/gyro.cfg", "r");
	char key[64];
	float v;

	if (f == NULL)
		return;
	while (fscanf(f, "%63s %f", key, &v) == 2) {
		if      (!strcmp(key, "enabled"))  cfg.enabled  = (int)v;
		else if (!strcmp(key, "yaw"))      cfg.yaw      = v;
		else if (!strcmp(key, "tilt"))     cfg.tilt     = v;
		else if (!strcmp(key, "pitch"))    cfg.pitch    = v;
		else if (!strcmp(key, "wheel"))    cfg.wheel    = v;
		else if (!strcmp(key, "drift"))    cfg.drift    = (int)v;
		else if (!strcmp(key, "cpd"))      cfg.cpd      = v;
		else if (!strcmp(key, "deadzone")) cfg.deadzone = v;
		else if (!strcmp(key, "invyaw"))   cfg.invYaw   = (int)v;
		else if (!strcmp(key, "invpitch")) cfg.invPitch = (int)v;
		else if (!strcmp(key, "verbose"))  cfg.verbose  = (int)v;
	}
	fclose(f);
}

void NX_GyroInit(void)
{
	load_cfg();
	printf("gyro: enabled=%d yaw=%.2f tilt=%.2f wheel=%.2f pitch=%.2f drift=%d cpd=%.2f dz=%.2f invYaw=%d invPitch=%d\n",
	       cfg.enabled, cfg.yaw, cfg.tilt, cfg.wheel, cfg.pitch, cfg.drift, cfg.cpd, cfg.deadzone, cfg.invYaw, cfg.invPitch);
}

/* Pick the sensor for whatever controller is active right now. Handheld and
 * FullKey take exactly 1 handle, only JoyDual takes 2 (asking for 2 on
 * handheld fails, and JoyDual while docked "succeeds" with dead handles). */
static void select_sensor(void)
{
	HidNpadIdType id;
	HidNpadStyleTag style;
	int count = 1, i, found = 0;
	u32 hh = hidGetNpadStyleSet(HidNpadIdType_Handheld);
	u32 n1 = hidGetNpadStyleSet(HidNpadIdType_No1);

	if (hh & HidNpadStyleTag_NpadHandheld) {
		id = HidNpadIdType_Handheld; style = HidNpadStyleTag_NpadHandheld; found = 1;
	} else if (n1 & HidNpadStyleTag_NpadFullKey) {
		id = HidNpadIdType_No1; style = HidNpadStyleTag_NpadFullKey; found = 1;
	} else if (n1 & HidNpadStyleTag_NpadJoyDual) {
		id = HidNpadIdType_No1; style = HidNpadStyleTag_NpadJoyDual; count = 2; found = 1;
	}

	if (!found) {
		haveHandle = 0;
		return;
	}
	if (haveHandle && id == curId && style == curStyle)
		return;

	memset(handles, 0, sizeof(handles));
	if (R_FAILED(hidGetSixAxisSensorHandles(handles, count, id, style))) {
		haveHandle = 0;
		return;
	}
	for (i = 0; i < count; i++) {
		hidStartSixAxisSensor(handles[i]);
		hidSetGyroscopeZeroDriftMode(handles[i], (HidGyroscopeZeroDriftMode)cfg.drift);
	}
	curId = id;
	curStyle = style;
	haveHandle = 1;
	lastSample = 0;
	printf("gyro: sensor style=0x%x id=%d handles=%d\n", (unsigned)style, (int)id, count);
}

/* Returns whole mouse counts to add this frame. frameSeconds = real frame time. */
void NX_GyroRead(float frameSeconds, int *dx, int *dy)
{
	HidSixAxisSensorState st[MAXSAMPLES];
	float avx = 0, avy = 0, avz = 0, acx = 0, acy = 0, acz = 0;
	int n, i, fresh = 0;
	u64 newest = lastSample;

	*dx = *dy = 0;
	if (!cfg.enabled)
		return;

	/* controller mode can change at any time (dock, detach) */
	if (!haveHandle || (frames++ & 63) == 0)
		select_sensor();
	if (!haveHandle)
		return;

	n = hidGetSixAxisSensorStates(handles[0], st, MAXSAMPLES);
	/* states are newest first */
	for (i = n - 1; i >= 0; i--) {
		if (st[i].sampling_number <= lastSample)
			continue;
		avx += st[i].angular_velocity.x; avy += st[i].angular_velocity.y; avz += st[i].angular_velocity.z;
		acx += st[i].acceleration.x;     acy += st[i].acceleration.y;     acz += st[i].acceleration.z;
		if (st[i].sampling_number > newest)
			newest = st[i].sampling_number;
		fresh++;
	}
	if (n > 0 && lastSample == 0)
		fresh = 0; /* first read after (re)select: just remember the position */
	lastSample = newest;
	if (fresh == 0)
		return;

	avx /= fresh; avy /= fresh; avz /= fresh;
	acx /= fresh; acy /= fresh; acz /= fresh;

	{
		/* libnx reports rotations per second */
		float ax = avx * 360.0f, ay = avy * 360.0f, az = avz * 360.0f;
		float am = sqrtf(acx * acx + acy * acy + acz * acz);
		float yawRate = 0, tiltRate = -ay, wheelRate = -az, pitchRate = ax; /* -ay: same sign as the gravity projection when the screen faces you, so they add */
		double ddx, ddy;

		if (am > 0.2f)
			yawRate = (ax * acx + ay * acy + az * acz) / am;

		if (fabsf(yawRate) < cfg.deadzone)   yawRate = 0;
		if (fabsf(tiltRate) < cfg.deadzone)  tiltRate = 0;
		if (fabsf(wheelRate) < cfg.deadzone) wheelRate = 0;
		if (fabsf(pitchRate) < cfg.deadzone) pitchRate = 0;

		if (cfg.invYaw)   { yawRate = -yawRate; tiltRate = -tiltRate; wheelRate = -wheelRate; }
		if (cfg.invPitch) pitchRate = -pitchRate;

		ddx = (yawRate * cfg.yaw + tiltRate * cfg.tilt + wheelRate * cfg.wheel) * frameSeconds * cfg.cpd + remX;
		ddy = (pitchRate * cfg.pitch) * frameSeconds * cfg.cpd + remY;
		*dx = (int)ddx;  remX = ddx - *dx;
		*dy = (int)ddy;  remY = ddy - *dy;

		if (cfg.verbose) {
			accumYaw += yawRate; accumPitch += pitchRate;
			if (++logTimer >= 120) {
				printf("gyro: n=%d yaw=%.1f tilt=%.1f pitch=%.1f |acc|=%.2f dx=%d dy=%d\n",
				       fresh, yawRate, tiltRate, pitchRate, am, *dx, *dy);
				logTimer = 0;
			}
		}
	}
}

/* R3: returns the new state */
int NX_GyroToggle(void)
{
	cfg.enabled = !cfg.enabled;
	remX = remY = 0;
	lastSample = 0;
	printf("gyro: toggled %s\n", cfg.enabled ? "ON" : "OFF");
	return cfg.enabled;
}
