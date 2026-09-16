module;

#include "Platform.h"
#include <limits>
#include <span>
#include <wincodec.h>

export module lemonsquash.bitmap;

export namespace Lemonsquash {
    class Bitmap {
        static constexpr unsigned int MaxDimension = 8192;

        unsigned int width = 0;
        unsigned int height = 0;
        std::vector<uint8_t> pixels;

        static IWICImagingFactory* ImagingFactory() {
            static auto* const factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory).detach();

            return factory;
        }

        static UINT BitmapByteSize(unsigned width, unsigned height) {
            if (!width || !height || width > std::numeric_limits<UINT>::max() / 4 / height)
                throw std::invalid_argument("Invalid bitmap dimensions");

            return width * height * 4;
        }

    public:
        static Bitmap LoadFromResource(HINSTANCE instance, int resourceId) {
            auto resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);

            if (!resource)
                throw std::runtime_error("Bitmap resource is missing");

            auto loaded = LoadResource(instance, resource);

            if (!loaded)
                throw std::runtime_error("Cannot load bitmap resource");

            auto bytes = static_cast<BYTE*>(LockResource(loaded));

            if (!bytes)
                throw std::runtime_error("Cannot read bitmap resource");

            auto* factory = ImagingFactory();

            winrt::com_ptr<IWICStream> stream;
            winrt::check_hresult(factory->CreateStream(stream.put()));
            winrt::check_hresult(stream->InitializeFromMemory(bytes, SizeofResource(instance, resource)));

            winrt::com_ptr<IWICBitmapDecoder> decoder;
            winrt::check_hresult(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put()));

            winrt::com_ptr<IWICBitmapFrameDecode> frame;
            winrt::check_hresult(decoder->GetFrame(0, frame.put()));

            return FromWicSource(frame.get());
        }

        static Bitmap FromGdiBitmap(HBITMAP image) {
            if (!image)
                throw std::invalid_argument("Bitmap handle is required");

            winrt::com_ptr<IWICBitmap> bitmap;
            winrt::check_hresult(ImagingFactory()->CreateBitmapFromHBITMAP(image, nullptr, WICBitmapUseAlpha, bitmap.put()));

            return FromWicSource(bitmap.get());
        }

        static Bitmap FromWicSource(IWICBitmapSource* source) {
            if (!source)
                throw std::invalid_argument("Bitmap source is required");

            winrt::com_ptr<IWICFormatConverter> converter;
            winrt::check_hresult(ImagingFactory()->CreateFormatConverter(converter.put()));
            winrt::check_hresult(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));

            Bitmap bitmap;

            winrt::check_hresult(converter->GetSize(&bitmap.width, &bitmap.height));

            if (bitmap.width > MaxDimension || bitmap.height > MaxDimension)
                throw std::runtime_error("Bitmap exceeds the size limit");

            UINT byteSize = BitmapByteSize(bitmap.width, bitmap.height);

            bitmap.pixels.resize(byteSize);

            winrt::check_hresult(converter->CopyPixels(nullptr, bitmap.width * 4, byteSize, bitmap.pixels.data()));

            return bitmap;
        }

        unsigned int Width() const noexcept {
            return width;
        }

        unsigned int Height() const noexcept {
            return height;
        }

        std::span<const uint8_t> Pixels() const noexcept {
            return {pixels.data(), pixels.size()};
        }

        HBITMAP ToGdiBitmap() const {
            UINT byteSize = BitmapByteSize(width, height);

            if (pixels.size() != byteSize)
                throw std::invalid_argument("Bitmap pixel buffer has an invalid size");

            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(width);
            info.bmiHeader.biHeight = -static_cast<LONG>(height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            info.bmiHeader.biSizeImage = byteSize;

            void* memory = nullptr;
            auto image = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &memory, nullptr, 0);

            if (!image || !memory) {
                if (image)
                    DeleteObject(image);

                throw std::runtime_error("Cannot create GDI bitmap");
            }

            memcpy(memory, pixels.data(), byteSize);

            return image;
        }
    };
}
