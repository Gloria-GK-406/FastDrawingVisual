#include "BatchComplier.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stringapiset.h>

namespace fdv::nativeproxy::shared::batch {
	namespace {

		struct ColorF {
			float r;
			float g;
			float b;
			float a;
		};

		bool Utf8ToWide(const std::uint8_t* bytes, std::uint32_t count,
			std::wstring& out) {
			out.clear();
			if (count == 0) {
				return true;
			}

			const int wideCount = MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(bytes),
				static_cast<int>(count), nullptr, 0);
			if (wideCount <= 0) {
				return false;
			}

			out.resize(static_cast<std::size_t>(wideCount));
			const int converted = MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(bytes),
				static_cast<int>(count), out.data(), wideCount);
			if (converted <= 0) {
				out.clear();
				return false;
			}

			return true;
		}

		ColorF ToPremultipliedColor(const fdv::protocol::ColorArgb8& color) {
			const float a = static_cast<float>(color.a) / 255.0f;
			return {
				(static_cast<float>(color.r) / 255.0f) * a,
				(static_cast<float>(color.g) / 255.0f) * a,
				(static_cast<float>(color.b) / 255.0f) * a,
				a,
			};
		}

		ColorF TransparentColor() { return { 0.0f, 0.0f, 0.0f, 0.0f }; }

		float ToShapeTypeValue(ShapeInstanceType type) {
			return static_cast<float>(static_cast<std::uint32_t>(type));
		}

		ShapeInstance MakeShapeInstance(float x, float y, float width, float height,
			float data0x, float data0y, float data0z,
			float data0w, const ColorF& fillColor,
			const ColorF& strokeColor, float strokeWidth,
			float radius, ShapeInstanceType type,
			float flags = 0.0f) {
			return {
				x,
				y,
				width,
				height,
				data0x,
				data0y,
				data0z,
				data0w,
				fillColor.r,
				fillColor.g,
				fillColor.b,
				fillColor.a,
				strokeColor.r,
				strokeColor.g,
				strokeColor.b,
				strokeColor.a,
				strokeWidth,
				radius,
				ToShapeTypeValue(type),
				flags,
			};
		}

		ShapeInstance MakeFillRectInstance(const fdv::protocol::FillRectPayload& payload) {
			return MakeShapeInstance(payload.x, payload.y, payload.width, payload.height,
				0.0f, 0.0f, 0.0f, 0.0f,
				ToPremultipliedColor(payload.color),
				TransparentColor(), 0.0f, 0.0f,
				ShapeInstanceType::FillRect);
		}

		ShapeInstance MakeFillRectInstance(const std::uint8_t* command) {
			return MakeShapeInstance(
				fdv::protocol::ReadF32(command + fdv::protocol::kFillRectXOffset),
				fdv::protocol::ReadF32(command + fdv::protocol::kFillRectYOffset),
				fdv::protocol::ReadF32(command + fdv::protocol::kFillRectWidthOffset),
				fdv::protocol::ReadF32(command + fdv::protocol::kFillRectHeightOffset),
				0.0f, 0.0f, 0.0f, 0.0f,
				ToPremultipliedColor(
					fdv::protocol::ReadColorArgb8(command +
						fdv::protocol::kFillRectColorOffset)),
				TransparentColor(), 0.0f, 0.0f, ShapeInstanceType::FillRect);
		}

		ShapeInstance MakeStrokeRectInstance(
			const fdv::protocol::StrokeRectPayload& payload) {
			return MakeShapeInstance(payload.x, payload.y, payload.width, payload.height,
				0.0f, 0.0f, 0.0f, 0.0f, TransparentColor(),
				ToPremultipliedColor(payload.color),
				(std::max)(1.0f, payload.thickness), 0.0f,
				ShapeInstanceType::StrokeRect);
		}

		ShapeInstance MakeStrokeRectInstance(const std::uint8_t* command) {
			return MakeShapeInstance(
				fdv::protocol::ReadF32(command + fdv::protocol::kStrokeRectXOffset),
				fdv::protocol::ReadF32(command + fdv::protocol::kStrokeRectYOffset),
				fdv::protocol::ReadF32(command + fdv::protocol::kStrokeRectWidthOffset),
				fdv::protocol::ReadF32(command + fdv::protocol::kStrokeRectHeightOffset),
				0.0f, 0.0f, 0.0f, 0.0f, TransparentColor(),
				ToPremultipliedColor(
					fdv::protocol::ReadColorArgb8(command +
						fdv::protocol::kStrokeRectColorOffset)),
				(std::max)(1.0f, fdv::protocol::ReadF32(
					command + fdv::protocol::kStrokeRectThicknessOffset)),
				0.0f, ShapeInstanceType::StrokeRect);
		}

		ShapeInstance MakeFillEllipseInstance(
			const fdv::protocol::FillEllipsePayload& payload) {
			return MakeShapeInstance(payload.centerX - payload.radiusX,
				payload.centerY - payload.radiusY,
				payload.radiusX * 2.0f, payload.radiusY * 2.0f,
				0.0f, 0.0f, 0.0f, 0.0f,
				ToPremultipliedColor(payload.color),
				TransparentColor(), 0.0f, 0.0f,
				ShapeInstanceType::FillEllipse);
		}

		ShapeInstance MakeFillEllipseInstance(const std::uint8_t* command) {
			const float centerX =
				fdv::protocol::ReadF32(command + fdv::protocol::kFillEllipseCenterXOffset);
			const float centerY =
				fdv::protocol::ReadF32(command + fdv::protocol::kFillEllipseCenterYOffset);
			const float radiusX =
				fdv::protocol::ReadF32(command + fdv::protocol::kFillEllipseRadiusXOffset);
			const float radiusY =
				fdv::protocol::ReadF32(command + fdv::protocol::kFillEllipseRadiusYOffset);
			return MakeShapeInstance(
				centerX - radiusX, centerY - radiusY, radiusX * 2.0f, radiusY * 2.0f,
				0.0f, 0.0f, 0.0f, 0.0f,
				ToPremultipliedColor(
					fdv::protocol::ReadColorArgb8(command +
						fdv::protocol::kFillEllipseColorOffset)),
				TransparentColor(), 0.0f, 0.0f, ShapeInstanceType::FillEllipse);
		}

		ShapeInstance MakeStrokeEllipseInstance(
			const fdv::protocol::StrokeEllipsePayload& payload) {
			const float thickness = (std::max)(1.0f, payload.thickness);
			const float outerRadiusX = payload.radiusX + thickness * 0.5f;
			const float outerRadiusY = payload.radiusY + thickness * 0.5f;
			return MakeShapeInstance(
				payload.centerX - outerRadiusX, payload.centerY - outerRadiusY,
				outerRadiusX * 2.0f, outerRadiusY * 2.0f, 0.0f, 0.0f, 0.0f, 0.0f,
				TransparentColor(), ToPremultipliedColor(payload.color), thickness, 0.0f,
				ShapeInstanceType::StrokeEllipse);
		}

		ShapeInstance MakeStrokeEllipseInstance(const std::uint8_t* command) {
			const float centerX = fdv::protocol::ReadF32(
				command + fdv::protocol::kStrokeEllipseCenterXOffset);
			const float centerY = fdv::protocol::ReadF32(
				command + fdv::protocol::kStrokeEllipseCenterYOffset);
			const float radiusX = fdv::protocol::ReadF32(
				command + fdv::protocol::kStrokeEllipseRadiusXOffset);
			const float radiusY = fdv::protocol::ReadF32(
				command + fdv::protocol::kStrokeEllipseRadiusYOffset);
			const float thickness =
				(std::max)(1.0f, fdv::protocol::ReadF32(
					command +
					fdv::protocol::kStrokeEllipseThicknessOffset));
			const float outerRadiusX = radiusX + thickness * 0.5f;
			const float outerRadiusY = radiusY + thickness * 0.5f;
			return MakeShapeInstance(
				centerX - outerRadiusX, centerY - outerRadiusY, outerRadiusX * 2.0f,
				outerRadiusY * 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, TransparentColor(),
				ToPremultipliedColor(fdv::protocol::ReadColorArgb8(
					command + fdv::protocol::kStrokeEllipseColorOffset)),
				thickness, 0.0f, ShapeInstanceType::StrokeEllipse);
		}

		ShapeInstance MakeLineInstance(const fdv::protocol::LinePayload& payload) {
			const float thickness = (std::max)(1.0f, payload.thickness);
			const float halfThickness = thickness * 0.5f;
			const float minX = (std::min)(payload.x0, payload.x1) - halfThickness;
			const float minY = (std::min)(payload.y0, payload.y1) - halfThickness;
			const float maxX = (std::max)(payload.x0, payload.x1) + halfThickness;
			const float maxY = (std::max)(payload.y0, payload.y1) + halfThickness;
			const float centerX = (minX + maxX) * 0.5f;
			const float centerY = (minY + maxY) * 0.5f;
			return MakeShapeInstance(
				minX, minY, maxX - minX, maxY - minY, payload.x0 - centerX,
				payload.y0 - centerY, payload.x1 - centerX, payload.y1 - centerY,
				TransparentColor(), ToPremultipliedColor(payload.color), thickness, 0.0f,
				ShapeInstanceType::Line);
		}

		ShapeInstance MakeLineInstance(const std::uint8_t* command) {
			const float x0 = fdv::protocol::ReadF32(command + fdv::protocol::kLineX0Offset);
			const float y0 = fdv::protocol::ReadF32(command + fdv::protocol::kLineY0Offset);
			const float x1 = fdv::protocol::ReadF32(command + fdv::protocol::kLineX1Offset);
			const float y1 = fdv::protocol::ReadF32(command + fdv::protocol::kLineY1Offset);
			const float thickness =
				(std::max)(1.0f, fdv::protocol::ReadF32(command +
					fdv::protocol::kLineThicknessOffset));
			const float halfThickness = thickness * 0.5f;
			const float minX = (std::min)(x0, x1) - halfThickness;
			const float minY = (std::min)(y0, y1) - halfThickness;
			const float maxX = (std::max)(x0, x1) + halfThickness;
			const float maxY = (std::max)(y0, y1) + halfThickness;
			const float centerX = (minX + maxX) * 0.5f;
			const float centerY = (minY + maxY) * 0.5f;
			return MakeShapeInstance(
				minX, minY, maxX - minX, maxY - minY, x0 - centerX, y0 - centerY,
				x1 - centerX, y1 - centerY, TransparentColor(),
				ToPremultipliedColor(
					fdv::protocol::ReadColorArgb8(command +
						fdv::protocol::kLineColorOffset)),
				thickness, 0.0f, ShapeInstanceType::Line);
		}

		fdv::protocol::DrawTextRunPayload ReadDrawTextRunPayload(
			const std::uint8_t* command) {
			fdv::protocol::DrawTextRunPayload payload{};
			payload.x = fdv::protocol::ReadF32(command + fdv::protocol::kDrawTextRunXOffset);
			payload.y = fdv::protocol::ReadF32(command + fdv::protocol::kDrawTextRunYOffset);
			payload.fontSize =
				fdv::protocol::ReadF32(command + fdv::protocol::kDrawTextRunFontSizeOffset);
			payload.color =
				fdv::protocol::ReadColorArgb8(command + fdv::protocol::kDrawTextRunColorOffset);
			payload.textUtf8 =
				fdv::protocol::ReadBlobRef(command + fdv::protocol::kDrawTextRunTextUtf8Offset);
			payload.fontFamilyUtf8 = fdv::protocol::ReadBlobRef(
				command + fdv::protocol::kDrawTextRunFontFamilyUtf8Offset);
			return payload;
		}

		double DurationMs(const std::chrono::steady_clock::time_point& start,
			const std::chrono::steady_clock::time_point& end) {
			return std::chrono::duration<double, std::milli>(end - start).count();
		}

	} // namespace

	void BatchCompiler::Reset(int width, int height, const void* commands,
		int commandBytes, const void* blobs, int blobBytes) {
		reader_.emplace(commands, commandBytes, blobs, blobBytes);
		width_ = width;
		height_ = height;
		widthF_ = static_cast<float>(width);
		heightF_ = static_cast<float>(height);
		lastBatchStats_ = {};
		if (commandBytes > 0) {
			const std::size_t shapeCapacityHint =
				static_cast<std::size_t>(commandBytes / fdv::protocol::kSlotBytes);
			if (shapeInstances_.capacity() < shapeCapacityHint) {
				shapeInstances_.reserve(shapeCapacityHint);
			}

			const std::size_t textCapacityHint = static_cast<std::size_t>(
				commandBytes / fdv::protocol::kDrawTextRunCommandBytes);
			if (textItems_.capacity() < textCapacityHint) {
				textItems_.reserve(textCapacityHint);
			}
		}
		shapeInstances_.clear();
		textItems_.clear();
	}

	HRESULT BatchCompiler::TryGetNextBatch(CompiledBatchView& out) {
		out = {};
		lastBatchStats_ = {};

		shapeInstances_.clear();
		textItems_.clear();

		fdv::protocol::RawCommandView command{};
		BatchKind kind = BatchKind::Unknown;

		while (true) {
			const auto readStart = std::chrono::steady_clock::now();
			const bool hasCommand = reader_->TryGetCurrentRaw(command);
			const auto readEnd = std::chrono::steady_clock::now();
			lastBatchStats_.commandReadMs += DurationMs(readStart, readEnd);

			if (!hasCommand) {
				lastBatchStats_.shapeInstanceCount =
					static_cast<int32_t>(shapeInstances_.size());
				lastBatchStats_.textItemCount =
					static_cast<int32_t>(textItems_.size());
				return kind == BatchKind::Unknown ? S_FALSE : S_OK;
			}

			switch (command.type) {
			case fdv::protocol::CommandType::Clear: {
				out.kind = BatchKind::Clear;
				lastBatchStats_.commands.clearCount++;

				const auto clearColor = ToPremultipliedColor(fdv::protocol::ReadColorArgb8(
					command.commandData + fdv::protocol::kClearColorOffset));
				out.clearColor[0] = clearColor.r;
				out.clearColor[1] = clearColor.g;
				out.clearColor[2] = clearColor.b;
				out.clearColor[3] = clearColor.a;
				reader_->Next();
				return S_OK;
			}

			case fdv::protocol::CommandType::FillRect:
				if (kind == BatchKind::Unknown) {
					kind = BatchKind::ShapeInstances;
					out.kind = kind;
				}
				else if (kind != BatchKind::ShapeInstances) {
					lastBatchStats_.shapeInstanceCount =
						static_cast<int32_t>(shapeInstances_.size());
					lastBatchStats_.textItemCount =
						static_cast<int32_t>(textItems_.size());
					return S_OK;
				}

				shapeInstances_.push_back(MakeFillRectInstance(command.commandData));
				lastBatchStats_.commands.fillRectCount++;
				reader_->Next();
				break;

			case fdv::protocol::CommandType::StrokeRect:
				if (kind == BatchKind::Unknown) {
					kind = BatchKind::ShapeInstances;
					out.kind = kind;
				}
				else if (kind != BatchKind::ShapeInstances) {
					lastBatchStats_.shapeInstanceCount =
						static_cast<int32_t>(shapeInstances_.size());
					lastBatchStats_.textItemCount =
						static_cast<int32_t>(textItems_.size());
					return S_OK;
				}

				shapeInstances_.push_back(MakeStrokeRectInstance(command.commandData));
				lastBatchStats_.commands.strokeRectCount++;
				reader_->Next();
				break;

			case fdv::protocol::CommandType::FillEllipse:
				if (kind == BatchKind::Unknown) {
					kind = BatchKind::ShapeInstances;
					out.kind = kind;
				}
				else if (kind != BatchKind::ShapeInstances) {
					lastBatchStats_.shapeInstanceCount =
						static_cast<int32_t>(shapeInstances_.size());
					lastBatchStats_.textItemCount =
						static_cast<int32_t>(textItems_.size());
					return S_OK;
				}

				shapeInstances_.push_back(MakeFillEllipseInstance(command.commandData));
				lastBatchStats_.commands.fillEllipseCount++;
				reader_->Next();
				break;

			case fdv::protocol::CommandType::StrokeEllipse:
				if (kind == BatchKind::Unknown) {
					kind = BatchKind::ShapeInstances;
					out.kind = kind;
				}
				else if (kind != BatchKind::ShapeInstances) {
					lastBatchStats_.shapeInstanceCount =
						static_cast<int32_t>(shapeInstances_.size());
					lastBatchStats_.textItemCount =
						static_cast<int32_t>(textItems_.size());
					return S_OK;
				}

				shapeInstances_.push_back(MakeStrokeEllipseInstance(command.commandData));
				lastBatchStats_.commands.strokeEllipseCount++;
				reader_->Next();
				break;

			case fdv::protocol::CommandType::Line:
				if (kind == BatchKind::Unknown) {
					kind = BatchKind::ShapeInstances;
					out.kind = kind;
				}
				else if (kind != BatchKind::ShapeInstances) {
					lastBatchStats_.shapeInstanceCount =
						static_cast<int32_t>(shapeInstances_.size());
					lastBatchStats_.textItemCount =
						static_cast<int32_t>(textItems_.size());
					return S_OK;
				}

				shapeInstances_.push_back(MakeLineInstance(command.commandData));
				lastBatchStats_.commands.lineCount++;
				reader_->Next();
				break;

			case fdv::protocol::CommandType::DrawTextRun: {
				if (kind == BatchKind::Unknown) {
					kind = BatchKind::Text;
					out.kind = kind;
				}
				else if (kind != BatchKind::Text) {
					lastBatchStats_.shapeInstanceCount =
						static_cast<int32_t>(shapeInstances_.size());
					lastBatchStats_.textItemCount =
						static_cast<int32_t>(textItems_.size());
					return S_OK;
				}

				const auto payload = ReadDrawTextRunPayload(command.commandData);
				fdv::protocol::BlobSpan textUtf8{};
				fdv::protocol::BlobSpan fontFamilyUtf8{};
				reader_->TryResolveBlob(payload.textUtf8, textUtf8);
				reader_->TryResolveBlob(payload.fontFamilyUtf8, fontFamilyUtf8);

				lastBatchStats_.commands.drawTextRunCount++;
				if (textUtf8.bytes != 0) {
					TextBatchItem item{};
					Utf8ToWide(textUtf8.data, textUtf8.bytes, item.text);
					Utf8ToWide(fontFamilyUtf8.data, fontFamilyUtf8.bytes, item.fontFamily);
					if (item.fontFamily.empty()) {
						item.fontFamily = L"Segoe UI";
					}

					item.fontSize = (std::max)(1.0f, payload.fontSize);
					item.layoutLeft = payload.x;
					item.layoutTop = payload.y;
					item.layoutRight = widthF_;
					item.layoutBottom = heightF_;
					item.color = payload.color;

					++lastBatchStats_.textItemCount;
					lastBatchStats_.textCharCount += static_cast<int32_t>(item.text.size());
					textItems_.push_back(std::move(item));
				}
				reader_->Next();
				break;
			}

			default:
				lastBatchStats_.shapeInstanceCount =
					static_cast<int32_t>(shapeInstances_.size());
				lastBatchStats_.textItemCount =
					static_cast<int32_t>(textItems_.size());
				return E_INVALIDARG;
			}
		}
	}

} // namespace fdv::nativeproxy::shared::batch
