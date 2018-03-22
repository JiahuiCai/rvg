#pragma once

#include <nytl/vec.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>

#include <variant>
#include <vector>
#include <cstddef>

namespace vgv {

using namespace nytl;

struct LineParams {};
struct SQBezierParams {};

struct QBezierParams { Vec2f control; };
struct SCBezierParams { Vec2f control2; };

struct CBezierParams {
	Vec2f control1;
	Vec2f control2;
};

struct ArcParams {
	Vec2f radius;
	bool largeArc {};
    bool clockwise {};
};

struct Command {
	Vec2f to;
	std::variant<
		LineParams,
		QBezierParams,
		SQBezierParams,
		CBezierParams,
		SCBezierParams,
		ArcParams> params;
};

struct Subpath {
	Vec2f start {};
	std::vector<Command> commands;
	bool closed {};
};

struct Path {
	std::vector<Subpath> subpaths;
};

std::vector<Vec2f> bake(const Subpath& sub);
Subpath parseSvgSubpath(nytl::Vec2f start, nytl::StringParam);

/// All information needed to represent a quadratic bezier curve.
struct QuadBezier {
	Vec2f start;
	Vec2f control;
	Vec2f end;
};

/// All information needed to represent a cubic bezier curve.
struct CubicBezier {
	Vec2f start;
	Vec2f control1;
	Vec2f control2;
	Vec2f end;
};

/// All information needed to draw an arc.
struct CenterArc {
	Vec2f center;
	Vec2f radius;
	float start;
	float end;
};

void bake(const CubicBezier&, std::vector<Vec2f>&, unsigned maxLevel = 8);
void bake(const QuadBezier&, std::vector<Vec2f>&, unsigned maxLevel = 10);
void bake(const CenterArc&, std::vector<Vec2f>&, unsigned steps);

CubicBezier quadToCubic(const QuadBezier&);
CenterArc parseArc(Vec2f from, const ArcParams&, Vec2f to);

/// Stroke api
enum class LineCap {
	butt = 0,
	round,
	square,
};

enum class LineJoin {
	miter = 0,
	round,
	bevel
};

/// Returns the points in triangle-strip form
std::vector<Vec2f> bakeStroke(Span<const Vec2f> points,
	float width, LineCap cap = LineCap::butt,
	LineJoin join = LineJoin::miter);

std::vector<Vec2f> bakeStroke(const Subpath& sub,
	float width, LineCap cap = LineCap::butt,
	LineJoin join = LineJoin::miter);

std::pair<std::vector<Vec2f>, std::vector<Vec2f>> bakeStrokeUv(
	Span<const Vec2f> points, float width, LineCap cap = LineCap::butt,
	LineJoin join = LineJoin::miter);

void bakeFillAA(Span<const Vec2f> points, float fringe,
	std::vector<Vec2f>& fillpos, std::vector<Vec2f>& strokepos,
	std::vector<Vec2f>& strokeuv);

} // namespace vgv
