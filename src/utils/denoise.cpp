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

#include <OpenImageDenoise/oidn.hpp>

MTS_NAMESPACE_BEGIN


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
        int optchar;
        std::string outputFilename;
        std::string albedoFilename;
        std::string normalFilename;

        /* Parse command-line arguments */
        while ((optchar = getopt(argc, argv, "htxag:m:f:r:b:c:o:p:s:B:F:")) != -1) {
            switch (optchar) {
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


        // Create an Open Image Denoise device
        oidn::DeviceRef device = oidn::newDevice();
        device.commit();

        /* Open files */
        ref<Bitmap> input  = loadImage(argv[optind]);
        Assert(input != nullptr);
        input = input->convert(outputPixelFormat, Bitmap::EFloat32);

        ref<Bitmap> albedo = loadImage(albedoFilename);
        if(albedo != nullptr) albedo = albedo->convert(outputPixelFormat, Bitmap::EFloat32);

        ref<Bitmap> normal = loadImage(normalFilename);
        if(albedo != nullptr) normal = normal->convert(outputPixelFormat, Bitmap::EFloat32);

        // Create the output file
        ref<Bitmap> output = input->clone();
        const int width  = input->getWidth();
        const int height = input->getHeight();

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
