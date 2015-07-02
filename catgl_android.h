//---------------------------------------------------------
//	catgl
//
//		©2015 Yuichiro Nakada
//---------------------------------------------------------

#include <jni.h>
#include <errno.h>

#include <EGL/egl.h>
#ifdef CATGL_GLES
#include <GLES/gl.h>
#else
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

#include <android/sensor.h>
#include <android/log.h>
#include <android_native_app_glue.h>

// デバッグ用メッセージ
#define TAG "catgl"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__))


// for OpenGL ES
#define glFrustum glFrustumf
#define glGenVertexArrays
#define glBindVertexArray


// アプリ動作再開に必要なデータ
struct saved_state {
	float angle; // 角度
	int32_t x;
	int32_t y;
};


extern void caInit(int width, int height);
extern void caRender();
extern void caEnd();

// アプリケーション内で共通して利用する情報
struct engine {
	struct android_app* app;

	// センサー関連
	ASensorManager* sensorManager;
	const ASensor* accelerometerSensor;  // 加速度センサー
	ASensorEventQueue* sensorEventQueue; // センサーイベントキュー

	// アニメーションフラグ
	int animating;

	// EGL
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;

	// 画面解像度
	int32_t width;
	int32_t height;

	// 保存データ
	struct saved_state state;
};

// EGL初期化
static int engine_init_display(struct engine* engine)
{
	EGLint w, h, dummy, format;
	EGLint numConfigs;
	EGLConfig config;
	EGLSurface surface;
	EGLContext context;

	// 有効にするEGLパラメータ
	const EGLint attribs[] = {
#ifndef CATGL_GLES
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
#endif
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_BLUE_SIZE,8, EGL_GREEN_SIZE,8, EGL_RED_SIZE,8, EGL_ALPHA_SIZE,8,
		EGL_STENCIL_SIZE,8,
		EGL_NONE
	};
	const EGLint contextAttribs[] = {
#ifndef CATGL_GLES
		EGL_CONTEXT_CLIENT_VERSION, 2,	// OpenGL ES2
#endif
		EGL_NONE
	};

	// EGLディスプレイコネクションを取得
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	// EGLディスプレイコネクション初期化
	eglInitialize(display, 0, 0);
	// 条件に合ったEGLフレームバッファ設定のリストを返す
	eglChooseConfig(display, attribs, &config, 1, &numConfigs);
	// EGLフレームバッファ設定の情報を取得
	eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);
	// NativeActivityへバッファを設定
	ANativeWindow_setBuffersGeometry(engine->app->window, 0, 0, format);
	// EGLウィンドウサーフェイスの取得
	surface = eglCreateWindowSurface(display, config, engine->app->window, NULL);
	// EGLレンダリングコンテキストの取得
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
	// EGLレンダリングコンテキストをEGLサーフェイスにアタッチする
	if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
		LOGW("Unable to eglMakeCurrent");
		return -1;
	}

	// 画面解像度取得
	eglQuerySurface(display, surface, EGL_WIDTH, &w);
	eglQuerySurface(display, surface, EGL_HEIGHT, &h);

	// EGL関連データの保存
	engine->display = display;
	engine->context = context;
	engine->surface = surface;

	// 画面解像度の保存
	engine->width = w;
	engine->height = h;

	// 矩形表示の初期化
	engine->state.angle = 0;
	caInit(engine->width, engine->height);

	return 0;
}

// 毎フレームの描画処理
static void engine_draw_frame(struct engine* engine)
{
	// displayが無い場合は描画しない
	if (engine->display == NULL) return;

	// 描画
	caRender();

	// ダブルバッファ入替
	eglSwapBuffers(engine->display, engine->surface);
}

// EGL情報を破棄する
static void engine_term_display(struct engine* engine)
{
	caEnd();

	if (engine->display != EGL_NO_DISPLAY) {
		// EGLレンダリングコンテキストとEGLサーフェイスの関連を外す
		eglMakeCurrent(engine->display,
		               EGL_NO_SURFACE,
		               EGL_NO_SURFACE,
		               EGL_NO_CONTEXT);
		// EGLレンダリングコンテキストを破棄する
		if (engine->context != EGL_NO_CONTEXT) {
			eglDestroyContext(engine->display, engine->context);
		}
		// EGLサーフェイスを破棄する
		if (engine->surface != EGL_NO_SURFACE) {
			eglDestroySurface(engine->display, engine->surface);
		}
		// EGLディスプレイを破棄する
		eglTerminate(engine->display);
	}
	// アニメーション停止
	engine->animating = 0;
	// EGL関連データを破棄
	engine->display = EGL_NO_DISPLAY;
	engine->context = EGL_NO_CONTEXT;
	engine->surface = EGL_NO_SURFACE;
}

// 入力イベントを処理する
static int32_t engine_handle_input(struct android_app* app, AInputEvent* event)
{
	// ユーザデータの取得
	struct engine* engine = (struct engine*) app->userData;
	if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
		// アニメーション有効化
		engine->animating = 1;
		// タッチ位置を取得
		engine->state.x = AMotionEvent_getX(event, 0);
		engine->state.y = AMotionEvent_getY(event, 0);
		return 1;
	}
	return 0;
}

// メインコマンドの処理
static void engine_handle_cmd(struct android_app* app, int32_t cmd)
{
	struct engine* engine = (struct engine*) app->userData;

	switch (cmd) {
	case APP_CMD_SAVE_STATE: // 状態保存を行うとき
		// 状態保存エリア取得
		engine->app->savedState = malloc(sizeof(struct saved_state));
		*((struct saved_state*) engine->app->savedState) = engine->state;
		engine->app->savedStateSize = sizeof(struct saved_state);
		break;

	case APP_CMD_INIT_WINDOW: // ウィンドウを初期化したとき
		if (engine->app->window != NULL) {
			// 画面を初期化する
			engine_init_display(engine);
			// 画面を描画する
			engine_draw_frame(engine);
		}
		break;

	case APP_CMD_TERM_WINDOW: // ウィンドウを破棄するとき
		// EGL情報を破棄する
		engine_term_display(engine);
		break;

	case APP_CMD_GAINED_FOCUS: // アプリがフォーカスを取得したとき
		if (engine->accelerometerSensor != NULL) {
			// 加速度センサーを有効化する
			ASensorEventQueue_enableSensor(engine->sensorEventQueue,
			                               engine->accelerometerSensor);
			// センサー情報取得間隔を設定する
			ASensorEventQueue_setEventRate(engine->sensorEventQueue,
			                               engine->accelerometerSensor, (1000L / 60) * 1000);
		}
		break;

	case APP_CMD_LOST_FOCUS: // フォーカスが消失したとき
		if (engine->accelerometerSensor != NULL) {
			// 加速度センサーを無効化する
			ASensorEventQueue_disableSensor(engine->sensorEventQueue,
			                                engine->accelerometerSensor);
		}
		// アニメーション停止
		engine->animating = 0;
		// 画面を表示
		engine_draw_frame(engine);
		break;
	}
}


// Main
void android_main(struct android_app* state)
{
	struct engine engine;

	// glueが削除されないように
	app_dummy();

	memset(&engine, 0, sizeof(engine));
	// ユーザーデータの配置
	state->userData = &engine;
	// コマンド処理関数の設定
	state->onAppCmd = engine_handle_cmd;
	// 入力イベント処理関数の設定
	state->onInputEvent = engine_handle_input;
	engine.app = state;

	// センサーマネージャの取得
	engine.sensorManager = ASensorManager_getInstance();
	// 加速度センサーのデータ取得準備
	engine.accelerometerSensor = ASensorManager_getDefaultSensor(engine.sensorManager, ASENSOR_TYPE_ACCELEROMETER);
	// センサー情報取得キューの新規作成
	engine.sensorEventQueue = ASensorManager_createEventQueue(engine.sensorManager, state->looper, LOOPER_ID_USER, NULL, NULL);

	if (state->savedState != NULL) {
		// 以前の状態に戻す
		engine.state = *(struct saved_state*) state->savedState;
	}

	while (1) {
		int ident;
		int events;
		struct android_poll_source* source;

		// センサー処理
		while ((ident = ALooper_pollAll(engine.animating ? 0 : -1, NULL, &events, (void**) &source)) >= 0) {

			// 内部イベントを処理する
			if (source != NULL) {
				source->process(state, source);
			}

			// センサー情報取得キューのデータを処理する
			if (ident == LOOPER_ID_USER) {
				if (engine.accelerometerSensor != NULL) {
					ASensorEvent event;
					while (ASensorEventQueue_getEvents(engine.sensorEventQueue, &event, 1) > 0) {
						LOGI("accelerometer: x=%f y=%f z=%f",
						     event.acceleration.x, event.acceleration.y,
						     event.acceleration.z);
					}
				}
			}

			// EGL情報を破棄する
			if (state->destroyRequested != 0) {
				engine_term_display(&engine);
				return;
			}
		}

		if (engine.animating) {
			// アニメーション（四角形の回転演算）処理
			engine.state.angle += .01f;
			if (engine.state.angle > 1) {
				engine.state.angle = 0;
			}

			// 画面の描画
			engine_draw_frame(&engine);
		}
	}
}