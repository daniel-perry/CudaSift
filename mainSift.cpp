//********************************************************//
// CUDA SIFT extractor by Marten Björkman aka Celebrandil //
//              celle @ nada.kth.se                       //
//********************************************************//  

#include <iostream>  
#include <cmath>
#include <iomanip>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "cudaImage.h"
#include "cudaSift.h"

int ImproveHomography(SiftData &data, float *homography, int numLoops, float minScore, float maxAmbiguity, float thresh);
double ComputeSingular(CudaImage *img, CudaImage *svd);
void PrintMatchData(SiftData &siftData1, SiftData &siftData2, CudaImage &img);
void MatchAll(SiftData &siftData1, SiftData &siftData2, float *homography);


#include "external/cpp-argparse/OptionParser.h"
optparse::OptionParser buildParser()
{
  const std::string usage = "%prog [OPTION]... left.pgm right.pgm out.pgm";
  const std::string version = "%prog 0.3";
  const std::string desc = "CUDA SIFT";
  const std::string epilog = "";

  optparse::OptionParser parser = optparse::OptionParser()
    .usage(usage)
    .version(version)
    .description(desc)
    .epilog(epilog);

  parser.add_option("--octaves").action("store").type("int").set_default(5).help("Number of octaves. Default 5.");
  parser.add_option("--initialblur").action("store").type("float").set_default(0.0).help("Initial blur. Default 0.0.");
  parser.add_option("--constrastthreshold").action("store").type("float").set_default(5.0).help("threshold for contrast, to minimize false positives. Default 5.0.");
  parser.add_option("--curvaturethreshold").action("store").type("float").set_default(16.0).help("threshold for curvature, to minimize false positives. Default 16.0.");
  parser.add_option("--descriptorthreshold").action("store").type("float").set_default(0.2).help("threshold for descriptor element magnitude, to minimize effect of illumination changes. Default 0.2.");
  parser.add_option("--matchratio").action("store").type("float").set_default(0.8).help("match ratio for finding matches. Default 0.8.");
  
  return parser;
}


///////////////////////////////////////////////////////////////////////////////
// Main program
///////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv) 
{     
  optparse::OptionParser parser = buildParser(); 
  optparse::Values & options = parser.parse_args(argc,argv);
  std::vector<std::string> args = parser.args();

  if(args.size() < 2)
  {
    parser.print_help();
    return 1;
  }
  std::string left_fn = args[0];
  std::string right_fn = args[1];
  std::string out_fn = args[2];

  // Read images using OpenCV
  cv::Mat limg, rimg;
  cv::imread(left_fn.c_str(), 0).convertTo(limg, CV_32FC1);
  cv::imread(right_fn.c_str(), 0).convertTo(rimg, CV_32FC1);
  unsigned int lw = limg.cols;
  unsigned int rw = rimg.cols;
  unsigned int lh = limg.rows;
  unsigned int rh = rimg.rows;
  std::cout << "Image size = (" << lw << "," << lh << ")" << std::endl;
  std::cout << "Image size = (" << rw << "," << rh << ")" << std::endl;
  
  // Perform some initial blurring (if needed)
  cv::GaussianBlur(limg, limg, cv::Size(5,5), 1.0);
  cv::GaussianBlur(rimg, rimg, cv::Size(5,5), 1.0);
        
  // Initial Cuda images and download images to device
  std::cout << "Initializing data..." << std::endl;
  InitCuda();
  CudaImage img1, img2;
  img1.Allocate(lw, lh, iAlignUp(lw, 128), false, NULL, (float*)limg.data);
  img2.Allocate(rw, rh, iAlignUp(rw, 128), false, NULL, (float*)rimg.data);
  img1.Download();
  img2.Download(); 

  // Extract Sift features from images
  SiftData siftData1, siftData2;
  float initBlur = options.get("initialblur"); // initial blur before constructing pyramid.
  float thresh = options.get("contrastthreshold"); // threshold for maxima (must be greater than this value).  = "threshold on feature contrast"
  float curvThresh = options.get("curvaturethreshold");
  int numOctaves = options.get("octaves");
  float descThresh = options.get("descriptorthreshold");
  float matchRatio = options.get("matchratio");
  //std::cerr << initBlur << " " << thresh << " " << curvThresh << " " << numOctaves << std::endl;
  InitSiftData(siftData1, 2048, true, true); 
  InitSiftData(siftData2, 2048, true, true);
  ExtractSift(siftData1, img1, numOctaves, initBlur, thresh, curvThresh, descThresh, 0.0f);
  ExtractSift(siftData2, img2, numOctaves, initBlur, thresh, curvThresh, descThresh, 0.0f);

  // Match Sift features and find a homography
  MatchSiftData(siftData1, siftData2);
  float homography[9];
  int numMatches;
  FindHomography(siftData1, homography, &numMatches, 10000, (0.50f/0.80f)*matchRatio, 1.00f, 5.0);
  int numFit = ImproveHomography(siftData1, homography, 3, matchRatio, 0.95f, 3.0);

  // Print out and store summary data
  PrintMatchData(siftData1, siftData2, img1);
#if 0
  PrintSiftData(siftData1);
  MatchAll(siftData1, siftData2, homography);
#endif
  std::cout << "Number of original features: " <<  siftData1.numPts << " " << siftData2.numPts << std::endl;
  float perc = 100.0f*numMatches/std::min(siftData1.numPts, siftData2.numPts);
  std::cout << "Number of matching features: " << numFit << " " << numMatches << " " << perc << "%" << std::endl;
  std::cerr << perc << std::endl;
  cv::imwrite(out_fn.c_str(), limg);

  // Free Sift data from device
  FreeSiftData(siftData1);
  FreeSiftData(siftData2);
}

void MatchAll(SiftData &siftData1, SiftData &siftData2, float *homography)
{
  SiftPoint *sift1 = siftData1.h_data;
  SiftPoint *sift2 = siftData2.h_data;
  int numPts1 = siftData1.numPts;
  int numPts2 = siftData2.numPts;
  int numFound = 0;
  for (int i=0;i<numPts1;i++) {
    float *data1 = sift1[i].data;
    std::cout << i << ":" << sift1[i].scale << ":" << (int)sift1[i].orientation << std::endl;
    bool found = false;
    for (int j=0;j<numPts2;j++) {
      float *data2 = sift2[j].data;
      float sum = 0.0f;
      for (int k=0;k<128;k++) 
	sum += data1[k]*data2[k];    
      float den = homography[6]*sift1[i].xpos + homography[7]*sift1[i].ypos + homography[8];
      float dx = (homography[0]*sift1[i].xpos + homography[1]*sift1[i].ypos + homography[2]) / den - sift2[j].xpos;
      float dy = (homography[3]*sift1[i].xpos + homography[4]*sift1[i].ypos + homography[5]) / den - sift2[j].ypos;
      float err = dx*dx + dy*dy;
      if (err<100.0f)
	found = true;
      if (err<100.0f || j==sift1[i].match) {
	if (j==sift1[i].match && err<100.0f)
	  std::cout << " *";
	else if (j==sift1[i].match) 
	  std::cout << " -";
	else if (err<100.0f)
	  std::cout << " +";
	else
	  std::cout << "  ";
	std::cout << j << ":" << sum << ":" << (int)sqrt(err) << ":" << sift2[j].scale << ":" << (int)sift2[j].orientation << std::endl;
      }
    }
    std::cout << std::endl;
    if (found)
      numFound++;
  }
  std::cout << "Number of founds: " << numFound << std::endl;
}

void PrintMatchData(SiftData &siftData1, SiftData &siftData2, CudaImage &img)
{
  int numPts = siftData1.numPts;
  SiftPoint *sift1 = siftData1.h_data;
  SiftPoint *sift2 = siftData2.h_data;
  float *h_img = img.h_data;
  int w = img.width;
  int h = img.height;
  std::cout << std::setprecision(3);
  for (int j=0;j<numPts;j++) { 
    int k = sift1[j].match;
    if (sift1[j].match_error<10) {
      float dx = sift2[k].xpos - sift1[j].xpos;
      float dy = sift2[k].ypos - sift1[j].ypos;
#if 0
      std::cout << j << ": " << "score=" << sift1[j].score << "  ambiguity=" << sift1[j].ambiguity << "  match=" << k << "  ";
      std::cout << "error=" << (int)sift1[j].match_error << "  ";
      std::cout << "orient=" << (int)sift1[j].orientation << "," << (int)sift2[k].orientation << "  ";
      std::cout << "pos1=(" << (int)sift1[j].xpos << "," << (int)sift1[j].ypos << ")" << std::endl;
      if (0) std::cout << "  delta=(" << (int)dx << "," << (int)dy << ")" << std::endl;
#endif
#if 1
      int len = (int)(fabs(dx)>fabs(dy) ? fabs(dx) : fabs(dy));
      for (int l=0;l<len;l++) {
	int x = (int)(sift1[j].xpos + dx*l/len);
	int y = (int)(sift1[j].ypos + dy*l/len);
	h_img[y*w+x] = 255.0f;
      }	
#endif
    }
#if 1
    int x = (int)(sift1[j].xpos+0.5);
    int y = (int)(sift1[j].ypos+0.5);
    int s = std::min(x, std::min(y, std::min(w-x-2, std::min(h-y-2, (int)(1.41*sift1[j].scale)))));
    int p = y*w + x;
    p += (w+1);
    for (int k=0;k<s;k++) 
      h_img[p-k] = h_img[p+k] = h_img[p-k*w] = h_img[p+k*w] = 0.0f;
    p -= (w+1);
    for (int k=0;k<s;k++) 
      h_img[p-k] = h_img[p+k] = h_img[p-k*w] =h_img[p+k*w] = 255.0f;
#endif
  }
  std::cout << std::setprecision(6);
}


