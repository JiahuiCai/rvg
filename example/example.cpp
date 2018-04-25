// Copyright (c) 2017 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

#include "render.hpp"
#include "window.hpp"

#include "vui/gui.hpp"
#include "vui/button.hpp"
#include "vui/window.hpp"
#include "vui/colorPicker.hpp"
#include "vui/textfield.hpp"

#include <rvg/context.hpp>
#include <rvg/shapes.hpp>
#include <rvg/paint.hpp>
#include <rvg/state.hpp>
#include <rvg/font.hpp>
#include <rvg/text.hpp>

#include <katachi/path.hpp>
#include <katachi/svg.hpp>

#include <ny/backend.hpp>
#include <ny/appContext.hpp>
#include <ny/key.hpp>
#include <ny/mouseButton.hpp>
#include <ny/event.hpp>

#include <vpp/instance.hpp>
#include <vpp/debug.hpp>
#include <vpp/formats.hpp>
#include <vpp/physicalDevice.hpp>

#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>

#include <dlg/dlg.hpp>

#include <chrono>
#include <array>

// using namespace vui;
using namespace nytl::vec::operators;
using namespace nytl::vec::cw::operators;

// settings
constexpr auto appName = "rvg-example";
constexpr auto engineName = "vpp,rvg";
constexpr auto useValidation = true;
constexpr auto startMsaa = vk::SampleCountBits::e1;
constexpr auto layerName = "VK_LAYER_LUNARG_standard_validation";
constexpr auto printFrames = true;
constexpr auto vsync = true;
constexpr auto clearColor = std::array<float, 4>{{0.f, 0.f, 0.f, 1.f}};

// TODO: move to nytl
template<typename T>
void scale(nytl::Mat4<T>& mat, nytl::Vec3<T> fac) {
	for(auto i = 0; i < 3; ++i) {
		mat[i][i] *= fac[i];
	}
}

template<typename T>
void translate(nytl::Mat4<T>& mat, nytl::Vec3<T> move) {
	for(auto i = 0; i < 3; ++i) {
		mat[i][3] += move[i];
	}
}

int main() {
	// - initialization -
	auto& backend = ny::Backend::choose();
	if(!backend.vulkan()) {
		throw std::runtime_error("ny backend has no vulkan support!");
	}

	auto appContext = backend.createAppContext();

	// vulkan init: instance
	auto iniExtensions = appContext->vulkanExtensions();
	iniExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

	vk::ApplicationInfo appInfo (appName, 1, engineName, 1, VK_API_VERSION_1_0);
	vk::InstanceCreateInfo instanceInfo;
	instanceInfo.pApplicationInfo = &appInfo;

	instanceInfo.enabledExtensionCount = iniExtensions.size();
	instanceInfo.ppEnabledExtensionNames = iniExtensions.data();

	if(useValidation) {
		instanceInfo.enabledLayerCount = 1;
		instanceInfo.ppEnabledLayerNames = &layerName;
	}

	vpp::Instance instance {};
	try {
		instance = {instanceInfo};
		if(!instance.vkInstance()) {
			throw std::runtime_error("vkCreateInstance returned a nullptr");
		}
	} catch(const std::exception& error) {
		dlg_error("Vulkan instance creation failed: {}", error.what());
		dlg_error("\tYour system may not support vulkan");
		dlg_error("\tThis application requires vulkan to work");
		throw;
	}

	// debug callback
	std::unique_ptr<vpp::DebugCallback> debugCallback;
	if(useValidation) {
		debugCallback = std::make_unique<vpp::DebugCallback>(instance);
	}

	// init ny window
	MainWindow window(*appContext, instance);
	auto vkSurf = window.vkSurface();

	// create device
	// enable some extra features
	float priorities[1] = {0.0};

	auto phdevs = vk::enumeratePhysicalDevices(instance);
	auto phdev = vpp::choose(phdevs, instance, vkSurf);

	auto queueFlags = vk::QueueBits::compute | vk::QueueBits::graphics;
	int queueFam = vpp::findQueueFamily(phdev, instance, vkSurf, queueFlags);

	vk::DeviceCreateInfo devInfo;
	vk::DeviceQueueCreateInfo queueInfo({}, queueFam, 1, priorities);

	auto exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	devInfo.pQueueCreateInfos = &queueInfo;
	devInfo.queueCreateInfoCount = 1u;
	devInfo.ppEnabledExtensionNames = exts.begin();
	devInfo.enabledExtensionCount = 1u;

	auto features = vk::PhysicalDeviceFeatures {};
	features.shaderClipDistance = true;
	devInfo.pEnabledFeatures = &features;

	auto device = vpp::Device(instance, phdev, devInfo);
	auto presentQueue = device.queue(queueFam);

	auto renderInfo = RendererCreateInfo {
		device, vkSurf, window.size(), *presentQueue,
		startMsaa, vsync, clearColor
	};

	auto renderer = Renderer(renderInfo);

	// rvg
	rvg::Context ctx(device, {renderer.renderPass(), 0, true});

	rvg::Transform transform(ctx);

	auto drawMode = rvg::DrawMode {false, 1.f};
	drawMode.aaStroke = true;
	drawMode.deviceLocal = true;
	rvg::Shape shape(ctx, {}, drawMode);
	rvg::Paint paint(ctx, rvg::colorPaint({rvg::norm, 0.1f, .6f, .3f}));

	auto fontHeight = 16;
	rvg::FontAtlas atlas(ctx);
	rvg::Font osFont(atlas, "../example/OpenSans-Regular.ttf", fontHeight);
	rvg::Font lsFont(atlas, "../example/LiberationSans-Regular.ttf", fontHeight);
	atlas.bake(ctx);

	rvg::Font lsSmall(atlas, "../example/LiberationSans-Regular.ttf", 14);
	atlas.bake(ctx);

	auto string = "yo, whaddup";
	rvg::Text text(ctx, string, lsFont, {0, 0});
	auto textWidth = lsFont.width(string);

	// svg path
	// auto svgSubpath = rvg::parseSvgSubpath({300, 200},
		// "h -150 a150 150 0 1 0 150 -150 z");
	auto svgSubpath = ktc::parseSvgSubpath("h 1920 v 1080 h -1920 z");

	rvg::Shape svgShape(ctx, ktc::flatten(svgSubpath), {true, 0.f});

	// image stuff
	rvg::RectShape foxRect(ctx, {500, 100}, {300, 200}, {true, 0.f});
	auto foxTex = rvg::Texture(ctx, "../example/fox.jpg");
	auto iv = foxTex.vkImageView();

	auto mat = nytl::identity<4, float>();
	mat[0][0] = 1 / 300.f;
	mat[1][1] = 1 / 200.f;
	mat[0][3] = -500.f / 300.f;
	mat[1][3] = -100.f / 200.f;
	rvg::Paint foxPaint = {ctx, rvg::texturePaintRGBA(mat, iv)};
	// rvg::Paint foxPaint = {ctx, rvg::colorPaint({150, 200, 200})};

	rvg::Paint svgPaint {ctx, rvg::colorPaint({150, 230, 200})};

	auto bgPaintData = rvg::colorPaint({5, 5, 5});
	auto labelPaintData = rvg::colorPaint({240, 240, 240});

	auto hintBgPaint = rvg::Paint(ctx, rvg::colorPaint({5, 5, 5, 200}));
	auto hintTextPaint = rvg::Paint(ctx, labelPaintData);

	auto bgPaint = rvg::Paint(ctx, bgPaintData);

	vui::Styles styles;

	// hint
	styles.hint.bg = &hintBgPaint;
	styles.hint.text = &hintTextPaint;
	styles.hint.font = &lsSmall;

	// button
	styles.button.normal.label = labelPaintData;
	styles.button.normal.bg = bgPaintData;

	styles.button.hovered.label = labelPaintData;
	styles.button.hovered.bg = rvg::colorPaint({20, 20, 20});

	styles.button.pressed.label = labelPaintData;
	styles.button.pressed.bg = rvg::colorPaint({35, 35, 35});

	// window
	styles.window.bg = &hintBgPaint;
	styles.window.rounding = {20.f, 20.f, 20.f, 20.f};
	styles.window.outerPadding = {20.f, 20.f};

	// textfield
	auto selectedPaint = rvg::Paint {ctx, rvg::colorPaint({50, 50, 50})};
	styles.textfield.bg = &bgPaint;
	styles.textfield.text = &hintTextPaint;
	styles.textfield.selected = &selectedPaint;
	styles.textfield.cursor = &hintTextPaint;

	// color picker
	styles.colorPicker.marker = &hintBgPaint;

	// colorButton
	styles.colorButton.bg = &hintBgPaint;

	// gui
	vui::Gui gui(ctx, lsFont, std::move(styles));
	auto& win = gui.create<vui::Window>(nytl::Rect2f {100, 100, 500, 880});
	auto& button = win.create<vui::Button>("button, waddup");
	button.onClick = [&](auto&) { dlg_info("Clicked!"); };
	auto& cp = win.create<vui::ColorPicker>();
	cp.onChange = [&](auto& cp){
		svgPaint.paint(rvg::colorPaint(cp.picked()));
	};

	win.create<vui::Button>("b#2");

	auto& tf = win.createSized<vui::Textfield>(nytl::Vec {400.f, vui::autoSize});
	tf.onChange = [&](auto& tf) {
		dlg_info("changed: {}", tf.utf8());
	};

	win.create<vui::ColorButton>();

	svgPaint = {ctx, rvg::colorPaint(cp.picked())};

	// gui
	/*
	auto& row = win.create<Row>(Vec2f{});
	row.create<Button>(Vec2f {}, "Row Button #1");
	row.create<Button>(Vec2f {}, "Row Button #2");

	auto colChangeFunc = [&](auto component) {
		return [&, component](float val) {
			auto col = svgPaint.paint.data.frag.inner;
			col.*component = 255.f * val;
			svgPaint.paint = rvg::colorPaint(col);
			if(svgPaint.updateDevice(ctx)) {
				dlg_warn("Unexpected rerecord");
			}

			cp.pick(col);
		};
	};

	auto& rslider = win.create<Slider>(Vec2f {}, 200);
	auto& gslider = win.create<Slider>(Vec2f {}, 200);
	auto& bslider = win.create<Slider>(Vec2f {}, 200);

	rslider.onChange = colChangeFunc(&rvg::Color::r);
	gslider.onChange = colChangeFunc(&rvg::Color::g);
	bslider.onChange = colChangeFunc(&rvg::Color::b);

	cp.onPick = [&](auto& cp) {
		svgPaint.paint = rvg::colorPaint(cp.picked);
		if(svgPaint.updateDevice(ctx)) {
			dlg_warn("Unexpected rerecord");
		}

		rslider.current(cp.picked.r / 255.f);
		gslider.current(cp.picked.g / 255.f);
		bslider.current(cp.picked.b / 255.f);
	};

	*/

	// render recoreding
	renderer.onRender += [&](vk::CommandBuffer buf){
		auto di = ctx.record(buf);

		transform.bind(di);
		svgPaint.bind(di);
		svgShape.fill(di);

		foxPaint.bind(di);
		foxRect.fill(di);

		paint.bind(di);
		shape.stroke(di);
		text.draw(di);

		gui.draw(di);
	};

	ctx.updateDevice();
	renderer.invalidate();

	// connect window & renderer
	auto run = true;
	window.onClose = [&](const auto&) { run = false; };
	window.onKey = [&](const auto& ev) {
		auto processed = false;
		processed |= (gui.key({(vui::Key) ev.keycode, ev.pressed}) != nullptr);
		if(ev.pressed && !ev.utf8.empty() && !ny::specialKey(ev.keycode)) {
			processed |= (gui.textInput({ev.utf8.c_str()}) != nullptr);
		}

		if(ev.pressed && !processed) {
			if(ev.keycode == ny::Keycode::escape) {
				dlg_info("Escape pressed, exiting");
				run = false;
			} else if(ev.keycode == ny::Keycode::b) {
				*paint.change() = rvg::colorPaint({rvg::norm, 0.2, 0.2, 0.8});
			} else if(ev.keycode == ny::Keycode::g) {
				*paint.change() = rvg::colorPaint({rvg::norm, 0.1, 0.6, 0.3});
			} else if(ev.keycode == ny::Keycode::r) {
				*paint.change() = rvg::colorPaint({rvg::norm, 0.8, 0.2, 0.3});
			} else if(ev.keycode == ny::Keycode::d) {
				*paint.change() = rvg::colorPaint({rvg::norm, 0.1, 0.1, 0.1});
			} else if(ev.keycode == ny::Keycode::w) {
				*paint.change() = rvg::colorPaint(rvg::Color::white);
			} else if(ev.keycode == ny::Keycode::p) {
				*paint.change() = rvg::linearGradient({0, 0}, {2000, 1000},
					{255, 0, 0}, {255, 255, 0});
			} else if(ev.keycode == ny::Keycode::c) {
				*paint.change() = rvg::radialGradient({1000, 500}, 0, 1000,
					{255, 0, 0}, {255, 255, 0});
			} else if(ev.keycode == ny::Keycode::k1) {
				text.change()->font = &lsFont;
			} else if(ev.keycode == ny::Keycode::k2) {
				text.change()->font = &osFont;
			}
		}
	};
	window.onResize = [&](const auto& ev) {
		renderer.resize(ev.size);

		auto tchange = text.change();
		tchange->position.x = (ev.size[0] - textWidth) / 2;
		tchange->position.y = ev.size[1] - fontHeight - 20;

		auto mat = nytl::identity<4, float>();
		auto s = nytl::Vec {2.f / window.size().x, 2.f / window.size().y, 1};
		scale(mat, s);
		translate(mat, {-1, -1, 0});
		*transform.change() = mat;
		gui.transform(mat);
	};

	ktc::Subpath subpath;
	bool first = true;

	window.onMouseButton = [&](const auto& ev) {
		auto p = static_cast<nytl::Vec2f>(ev.position);
		if(gui.mouseButton({ev.pressed,
				static_cast<vui::MouseButton>(ev.button), p})) {
			return;
		}

		if(!ev.pressed) {
			return;
		}

		if(ev.button == ny::MouseButton::left) {
			if(first) {
				first = false;
				subpath.start = p;
			} else {
				subpath.sqBezier(p);
				shape.change()->points = ktc::flatten(subpath);
			}
		} else if(ev.button == ny::MouseButton::right) {
			win.position(p);
		}
	};

	window.onMouseMove = [&](const auto& ev) {
		gui.mouseMove({static_cast<nytl::Vec2f>(ev.position)});
	};

	// - main loop -
	using Clock = std::chrono::high_resolution_clock;
	using Secf = std::chrono::duration<float, std::ratio<1, 1>>;

	auto lastFrame = Clock::now();
	auto fpsCounter = 0u;
	auto secCounter = 0.f;

	while(run) {
		auto now = Clock::now();
		auto diff = now - lastFrame;
		auto deltaCount = std::chrono::duration_cast<Secf>(diff).count();
		lastFrame = now;

		if(!appContext->pollEvents()) {
			dlg_info("pollEvents returned false");
			return 0;
		}

		gui.update(deltaCount);

		if(gui.updateDevice()) {
			dlg_info("gui rerecord");
			renderer.invalidate();
		}

		if(ctx.updateDevice()) {
			dlg_info("ctx rerecord");
			renderer.invalidate();
		}

		auto semaphore = ctx.stageUpload();
		auto wait = {
			vpp::StageSemaphore {
				semaphore,
				vk::PipelineStageBits::drawIndirect,
			}
		};

		vpp::RenderInfo info;
		if(semaphore) {
			info.wait = wait;
		}

		renderer.renderBlock(info);

		if(printFrames) {
			++fpsCounter;
			secCounter += deltaCount;
			if(secCounter >= 1.f) {
				dlg_info("{} fps", fpsCounter);
				secCounter = 0.f;
				fpsCounter = 0;
			}
		}
	}
}
