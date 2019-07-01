/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2014 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/render/util.h>
#include <mitsuba/core/timer.h>
#include <mitsuba/core/fresolver.h>
#include <mitsuba/core/fstream.h>
#include <mitsuba/core/bitmap.h>
#include <mitsuba/core/plugin.h>
#include <boost/algorithm/string.hpp>
#if defined(WIN32)
# include <mitsuba/core/getopt.h>
#endif
#ifdef MTS_OPENMP
# include <omp.h>
#endif

#ifdef USE_OIID
#include <OpenImageDenoise/oidn.hpp>
#endif

MTS_NAMESPACE_BEGIN

/* Denoising classes */

/* OpenImageDenoise */
struct OpenImageDenoise
{
};

/* Cross-Bilateral filter */
struct CrossBilateral
{
   CrossBilateral(ref<Bitmap>& image,
                  ref<Bitmap>& normal,
                  ref<Bitmap>& albedo,
                  ref<Bitmap>& depth)
      : m_image(image),
        m_normal(normal),
        m_albedo(albedo),
        m_depth(depth)
   {
   }

   void run(ref<Bitmap> output) const
   {
      const int width  = output->getWidth();
      const int height = output->getHeight();
      #pragma omp parallel for collapse(2)
      for(int i=0; i<width; ++i)
         for(int j=0; j<height; ++j)
         {
            denoise_pixel(output, i, j);
         }
   }

   inline
   void denoise_pixel(ref<Bitmap>& output, int i, int j) const
   {
      // Constants
      const int width  = output->getWidth();
      const int height = output->getHeight();

      const Point2i  p_ij      = Point2i(i,j);

      const Spectrum albedo_ij = m_albedo->getPixel( p_ij );
      const Spectrum normal_ij = m_normal->getPixel( p_ij );
      const Spectrum depth_ij  = m_depth->getPixel( p_ij );

      // Varying
      Spectrum cum_value(0.0f);
      float cum_weight = 0.0f;

      for(int di = -w; di<=w; ++di)
         for(int dj=-w; dj<=w; ++dj)
         {
            const int u = (i + di + width)  % width;
            const int v = (j + dj + height) % height;

            const Point2i p_uv = Point2i(u,v);

            const Spectrum albedo_uv = m_albedo->getPixel( p_uv );
            const Spectrum normal_uv = m_normal->getPixel( p_uv );
            const Spectrum depth_uv  = m_depth->getPixel( p_uv );

            Spectrum value  = m_image->getPixel( p_uv );
            Float    weight = 1.0f;

            weight *= exp( - m_inv_sigma_pixels * Float( di*di + dj*dj ) );
            weight *= exp( - m_inv_sigma_albedo * powf((albedo_ij - albedo_uv).average(), 2.0f) );
            weight *= exp( - m_inv_sigma_normal * powf((normal_ij - normal_uv).average(), 2.0f) );
            weight *= exp( - m_inv_sigma_depth * powf((depth_ij - depth_uv).average(), 2.0f) );

            cum_value  += weight*value;
            cum_weight += weight;
         }

      cum_value /= cum_weight;
      output->setPixel( p_ij, cum_value );
   }

   ref<Bitmap> m_image;
   ref<Bitmap> m_normal;
   ref<Bitmap> m_albedo;
   ref<Bitmap> m_depth;

   int w = 3;

   Float m_inv_sigma_pixels = 0.1;
   Float m_inv_sigma_albedo = 10.0;
   Float m_inv_sigma_depth  = 10.0;
   Float m_inv_sigma_normal = 10.0;

};


ref<Bitmap> loadImage(const std::string& filename) {

    if(filename.empty()) {
        return nullptr;
    }

    ref<FileResolver> fileResolver = Thread::getThread()->getFileResolver();
    fs::path file = fileResolver->resolve(filename);
    ref<FileStream> is = new FileStream(file, FileStream::EReadOnly);
    ref<Bitmap> input  = new Bitmap(Bitmap::EAuto, is);
    return input;
}

class Denoise : public Utility {
public:
    void help() {
        cout << endl;
        cout << "Synopsis: Loads one or more EXR/RGBE images and writes tonemapped 8-bit PNG/JPGs";
        cout << endl;
        cout << "Usage: mtsutil tonemap [options] -o output file" << endl;
        cout << "Options/Arguments:" << endl;
        cout << "   -h      Display this help text" << endl << endl;
        cout << "   -a      Albedo image filename" << endl << endl;
        cout << "   -n      Normal image filename" << endl << endl;
    }

    typedef struct {
        int r[5];
    } Rect;


    int run(int argc, char **argv) {
        ref<FileResolver> fileResolver = Thread::getThread()->getFileResolver();
        std::string outputFilename;
        std::string albedoFilename;
        std::string normalFilename;
        std::string depthFilename;

        /* Parse command-line arguments
         * First element in argv list is the name of the plugin `denoise`
         * Last element in argv list is the name of the input file
         */
        for(auto i=1; i<argc-1; ++i)
        {
           // Only consider the case of '-[letter] argument'
           if(argv[i][0] != '-')
              continue;

           char  opt    = (argv[i][1] == '-') ? argv[i][2] : argv[i][1];
           char* optarg = argv[i+1];
           switch (opt)
           {
              case 'h': {
                           help();
                           return 0;
                        }
                        break;

              case 'o':
                        outputFilename = std::string(optarg);
                        break;

              case 'a':
                        albedoFilename = std::string(optarg);
                        break;

              case 'n':
                        normalFilename = std::string(optarg);
                        break;

              case 'd':
                        depthFilename = std::string(optarg);
                        break;
            }
        }

        if(outputFilename.empty()) {
            SLog(EError, "No output file specified! Use `-o filename` in the command line");
        }

        /* I/O variables
         *
         * Here we check for the output format.
         * We only deal with image format mitsuba can export.
         * See bitmap.cpp:384.
         */
        auto extPosition = outputFilename.find_last_of('.');
        std::string outputExtension = outputFilename.substr(extPosition+1);
        auto outputFileFormat  = Bitmap::EOpenEXR;
        auto outputPixelFormat = Bitmap::ERGB;
        auto outputCompFormat  = Bitmap::EFloat32;
        if(outputExtension == "png") {
            outputFileFormat = Bitmap::EPNG;
            outputCompFormat = Bitmap::EUInt8;
        } else if(outputExtension == "jpg") {
            outputFileFormat = Bitmap::EJPEG;
            outputCompFormat = Bitmap::EUInt8;
        } else if(outputExtension == "ppm") {
            outputFileFormat = Bitmap::EPPM;
            outputCompFormat = Bitmap::EUInt8;
        } else if(outputExtension == "pfm") {
            outputFileFormat = Bitmap::EPFM;
            outputCompFormat = Bitmap::EFloat32;
        } else if(outputExtension == "rgbe") {
            outputFileFormat = Bitmap::ERGBE;
            outputCompFormat = Bitmap::EFloat32;
        } else {
            outputFilename.replace(extPosition+1, 3, "exr");
        }

        /* Open files */
        ref<Bitmap> input  = loadImage(argv[argc-1]);
        Assert(input != nullptr);
        input = input->convert(outputPixelFormat, Bitmap::EFloat32);

        ref<Bitmap> albedo = loadImage(albedoFilename);
        if(albedo != nullptr) albedo = albedo->convert(outputPixelFormat, Bitmap::EFloat32);

        ref<Bitmap> normal = loadImage(normalFilename);
        if(albedo != nullptr) normal = normal->convert(outputPixelFormat, Bitmap::EFloat32);

        ref<Bitmap> depth = loadImage(depthFilename);
        if(albedo != nullptr) depth = depth->convert(outputPixelFormat, Bitmap::EFloat32);

        // Create the output file
        ref<Bitmap> output = input->clone();
        const int width  = input->getWidth();
        const int height = input->getHeight();

#ifdef USE_OIID
        // Create an Open Image Denoise device
        oidn::DeviceRef device = oidn::newDevice();
        device.commit();

        // Create a denoising filter
        oidn::FilterRef filter = device.newFilter("RT"); // generic ray tracing filter
        if(albedo != nullptr) filter.setImage("albedo", albedo->getFloat32Data(), oidn::Format::Float3, width, height);
        if(normal != nullptr) filter.setImage("normal", normal->getFloat32Data(), oidn::Format::Float3, width, height);
        filter.setImage("color",  input->getFloat32Data(),  oidn::Format::Float3, width, height);
        filter.setImage("output", output->getFloat32Data(), oidn::Format::Float3, width, height);
        filter.set("hdr", outputCompFormat == Bitmap::EFloat32); // image is HDR
        filter.commit();

        // Filter the image
        filter.execute();

        // Check for errors
        const char* errorMessage;
        if (device.getError(errorMessage) != oidn::Error::None) {
            SLog(EError, errorMessage);
        }
#else
        CrossBilateral filter(input, albedo, normal, depth);
        filter.run(output);
#endif

        /* Output image */
        output = output->convert(outputPixelFormat, outputCompFormat, -1.0);
        ref<FileStream> os = new FileStream(outputFilename, FileStream::ETruncReadWrite);
        output->write(outputFileFormat, os);

        return 0;
    }

    MTS_DECLARE_UTILITY()
};

MTS_EXPORT_UTILITY(Denoise, "Command line batch denoiser")
MTS_NAMESPACE_END
