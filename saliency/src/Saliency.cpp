/*
 *      Author: Amir Rasouli
 *      email: aras@eecs.yorku.ca
 *
 *      This code is a ROS package for calculating a saliency map given an input image. The saliency is used in [1,2]
 *      for robotic visual search.
 *
 *      The code generates two types of salinecy:
 *
 *      Bottom-up Saliency: This model is based on AIM [3] which generates a bottom-up saliency map given an input image and a basis function.
 *      The C code of AIM is written by Iullia Kotseruba and is optimized on CPU for maximum processing speed. The contribution of the author is
 *      only in ROS wrapper.
 *
 *      Top-down Saliency: This model generates top-down saliency using histogram backproject. The saliency is based on the color distribution
 *      of the object. The inputs for this model are an input image and an object's template. In addition there are 20 different color spaces
 *      that can be used for color representation.
 *
 *   	[1] A. Rasouli and J. K. Tsotsos, "Attention in Autonomous Robotic Visual Search", in Proc. i-SAIRAS,  Jun. 2014
 *   	[2] A. Rasouli and J. K. Tsotsos, "Visual Saliency Improves Autonomous Visual Search", in Proc. The Conference on Computer and Robotic Vision, May, 2014
 *      [3]N. Bruce and J. K. Tsotsos, “Attention based on information maximization,”
 *       Journal of Vision, vol. 7, no. 9, p. 950, 2007.
 */
#include "Saliency.h"

using namespace cv;
using namespace std;



Saliency::Saliency()
{
	_colors =  vector<string>{"RGB", "HSV", "Lab", "Luv", "HSI", "HSL", "CMY", "C1C2C3", "COPP", "YCrCb", "YIQ", "XYZ", "UVW", "YUV",
		"OPP", "NOPP", "xyY", "rg", "YES", "I1I2I3"};
	namespace_ = "saliency";
	getAIMService = "getAIMService";
	getBackProjService = "getBackProjService";
	counter = 0;
	num_bins = 128;

	gotKernel = false;
	//this->loadBasis();

};
Saliency::~Saliency(){
	resetAIM();
};
//Resets the content of pointer arrays used In AIM
void Saliency::resetAIM()
{
	for (int i = 0; i < num_kernels; i++) {
		delete [] kernels[i];
	}
	delete [] kernels;
	delete [] aim_temp;
	delete [] data;
}

//****************************** Utilities ******************************
// Pixel-wise normalizes an image
cv::Mat Saliency::normalizeImage(cv::Mat RGBImage)
{
	Mat rgbImg = RGBImage.clone();

	double intensity = 0.;
	double nR = 0.;
	double nG = 0.;
	double nB = 0.;

	for (int i = 0; i < rgbImg.rows; i++){
		for (int j = 0; j < rgbImg.cols ; j++){
			intensity = rgbImg.at<Vec3b>(i,j)[0] + rgbImg.at<Vec3b>(i,j)[1] + rgbImg.at<Vec3b>(i,j)[2];
			if (intensity == 0.){
				intensity = 1/sqrt((double)3);
			}
			nB = rgbImg.at<Vec3b>(i,j)[0]/intensity;
			nG = rgbImg.at<Vec3b>(i,j)[1]/intensity;
			nR = rgbImg.at<Vec3b>(i,j)[2]/intensity;
			rgbImg.at<Vec3b>(i,j)[0] = floor(nB*255);
			rgbImg.at<Vec3b>(i,j)[1] = floor(nG*255);
			rgbImg.at<Vec3b>(i,j)[2] = floor(nR*255);
		}
	}


	return rgbImg ;

}
// Normalizes the color histogram
void Saliency::normalizeHistogram(cv::Mat &histogram)
{
	double minScale, maxScale;
	int minIdx[3], maxIdx[3];
	minMaxIdx(histogram, &minScale, &maxScale, minIdx, maxIdx);
	histogram -= minScale;
	histogram *= 255/(maxScale-minScale);

}
//Calculates the percentile value and uses it to threshold the input saliency map
cv::Mat Saliency::percentileThreshold(cv::Mat salMap, double percentile)
{
	Mat salMapBinary;
	Mat map = salMap.clone();
	Mat vectorizedMap = map.reshape(1,1);
	cv::sort(vectorizedMap , vectorizedMap , CV_SORT_ASCENDING);
	float ip, xInt, fPart, kPart;
	int n = vectorizedMap.cols;
	ip = (percentile/100)*(n+1);//(percentile*n)/100 + 0.5;
	fPart = modf(ip, &kPart);
	int h = (int)vectorizedMap.at<uchar>(0, kPart-1);
	int g = (int)vectorizedMap.at<uchar>(0, kPart);
	xInt = (1 - fPart)*h +fPart *g;
	threshold(salMap, salMapBinary, (double)xInt, 255, THRESH_TOZERO);
	return salMapBinary;
}
// Converts an RGB input image to 20 different color spaces
cv::Mat Saliency::imageConversion(cv::Mat inputImg, colorSpace type, bool norm)
{
	Mat output;
	vector<Mat> channels, HSIChannels, CMYChannels(3), C1C2C3Channels(3),
			O1O2Channels(2), YIQChannels(3), UVWChannels(3), YUVChannels(3),
			xyYChannels(3), OPPChannels(3), rgChannels(2), YESChannels(3), I3Channels(3);
	Mat t, I, S, scaledImage, onesMat;
	Mat Y, C, C1, C2, H, O3;
	vector<double> mean, dev;
	int index = type;

	inputImg.convertTo(scaledImage, CV_32FC3);
	scaledImage *= 1.f/255.f;
	cout << "Convert To color space: " << _colors[index] << "\n";

	switch (type){
	//******************* HSV****************
	case colorSpace::HSV:
		cvtColor(scaledImage, output, CV_BGR2HSV);
		if (norm){
			split(output, channels);
			channels[0]/=360;
			merge(channels,output);
		}

		break;

		//******************* HSL****************
	case colorSpace::HSL:
		cvtColor(scaledImage, output, CV_BGR2HLS);
		split(output, channels);
		t = channels[1].clone();
		channels[1] = channels[2].clone();
		channels[2] = t.clone();

		if(norm){
			channels[0]/=360;
		}

		merge(channels, output);
		break;

		//******************* HSI****************
	case colorSpace::HSI:
		cvtColor(scaledImage, t, CV_BGR2HLS);
		split(scaledImage, channels);

		I = 1.f/3.f * (channels[0] + channels[1]+ channels[2]);
		S = Mat(channels[0].rows, channels[0].cols, CV_32F); // s in [0,1]
		//.ptr<t>(rows)[cols*3]
		//.ptr<t>(i)[j*3+1]
		//.ptr<t>(i)[j*3+2]
		for (int i = 0; i < S.rows; i++)
			for(int j = 0; j< S.cols; j++)
			{
				float r = channels[2].at<float>(i,j);
				float g = channels[1].at<float>(i,j);
				float b = channels[0].at<float>(i,j);

				if (max(g,max(r,b)) != 0)
				{
					S.at<float>(i,j) = 1 - min(g,min(r,b))/I.at<float>(i,j);

				}else
				{
					S.at<float>(i,j) = 0.f;
				}
			}

		split(t, HSIChannels);
		HSIChannels[1] = S;
		HSIChannels[2] = I;

		if(norm)
		{
			HSIChannels[0]/=360.f;
		}

		merge(HSIChannels, output);
		break;

		//******************* CMY****************
	case colorSpace::CMY:
		split(scaledImage, channels);
		onesMat = Mat(scaledImage.rows, scaledImage.cols,CV_32F, Scalar::all(1));
		CMYChannels[0] = onesMat - channels[2];
		CMYChannels[1] = onesMat - channels[1];
		CMYChannels[2] = onesMat - channels[0];
		merge(CMYChannels, output);
		break;

		//******************* Lab****************
	case colorSpace::Lab:
		cvtColor(scaledImage , output , CV_BGR2Lab);

		if (norm)
		{
			split(output, channels);
			channels[0]/=100.f;
			channels[1] = (channels[1] + 127.f)/254.f;
			channels[2] = (channels[2] + 127.f)/254.f;
			merge(channels, output);
		}

		break;

		//******************* Luv ****************
	case colorSpace::Luv:
		cvtColor(scaledImage , output , CV_BGR2Luv);

		if (norm)
		{
			split(output, channels);
			channels[0]/=100.f;
			channels[1] = (channels[1] + 134.f)/354.f;
			channels[2] = (channels[2] + 140.f)/262.f;
			merge(channels, output);
		}

		break;

		//******************* YCrCb ****************
	case colorSpace::YCrCb:
		cvtColor(scaledImage , output , CV_BGR2YCrCb);
		break;

		//******************* C1C2C3 ****************
	case colorSpace::C1C2C3:
		split(scaledImage, channels);
		C1C2C3Channels[0] = Mat(scaledImage.rows, scaledImage.cols,CV_32F);
		C1C2C3Channels[1] = Mat(scaledImage.rows, scaledImage.cols,CV_32F);
		C1C2C3Channels[2] = Mat(scaledImage.rows, scaledImage.cols,CV_32F);

		for (int i = 0; i < scaledImage.rows; i++)
			for(int j = 0; j < scaledImage.cols; j++)
			{
				float r = channels[2].at<float>(i,j);
				float g = channels[1].at<float>(i,j);
				float b = channels[0].at<float>(i,j);

				C1C2C3Channels[0].at<float>(i,j) = atan2(r,max(g,b));
				C1C2C3Channels[1].at<float>(i,j) = atan2(g,max(r,b));
				C1C2C3Channels[2].at<float>(i,j) = atan2(b,max(g,r));
			}

		if (norm)
		{
			C1C2C3Channels[0] = (C1C2C3Channels[0]+PI/2.f)/PI;
			C1C2C3Channels[1] = (C1C2C3Channels[1]+PI/2.f)/PI;
			C1C2C3Channels[2] = (C1C2C3Channels[2]+PI/2.f)/PI;
		}

		merge(C1C2C3Channels, output);
		break;

		//******************* COPP ****************
	case colorSpace::COPP:
		split(scaledImage, channels);
		O1O2Channels[0] = (channels[2]-channels[1])/sqrt(2.f);
		O1O2Channels[1] = (channels[2]+channels[1]-2.f*channels[0])/sqrt(6.f);

		if (norm)
		{
			float denom = 1.f/sqrt(2.f);
			float denom2 = 2.f/sqrt(6.f);
			O1O2Channels[0] = (O1O2Channels[0]+denom )/(denom*2.f);
			threshold( O1O2Channels[0], O1O2Channels[0], 0, 0,THRESH_TOZERO);

			O1O2Channels[1] = (O1O2Channels[1]+denom2 )/(denom2*2.f);
			threshold( O1O2Channels[1], O1O2Channels[1], 0, 0,THRESH_TOZERO);
		}

		merge(O1O2Channels, output);
		break;

		//******************* XYZ ****************
	case colorSpace::XYZ:

		cvtColor(scaledImage , output , CV_BGR2XYZ);

		if (norm)
		{
			split(output, channels);
			channels[0] /= 0.950456f;
			channels[2] /= 1.088754f ;
			merge(channels, output);
		}

		break;

		//******************* YIQ ****************
	case colorSpace::YIQ:
		split(scaledImage, channels);
		YIQChannels[0] = 0.299f*channels[2] + 0.587f*channels[1] + 0.114f*channels[0];
		YIQChannels[1] = 0.596f*channels[2] - 0.274f*channels[1] - 0.322f*channels[0];
		YIQChannels[2] = 0.211f*channels[2] - 0.523f*channels[1] - 0.312f*channels[0];

		if (norm)
		{
			YIQChannels[1]  = (YIQChannels[1] + 0.596f)/(1.192f);
			YIQChannels[2]  = (YIQChannels[2] + 0.835f)/(1.046f);
		}

		merge(YIQChannels, output);
		break;

		//*******************  UVW ****************
	case colorSpace::UVW:
		cvtColor(scaledImage , t , CV_BGR2XYZ);
		split(t, channels);
		UVWChannels[0] = 0.66f*channels[0];
		UVWChannels[1] = channels[1];
		UVWChannels[2] = -0.5f*channels[0] + 1.5f*channels[1] + 0.5f*channels[2];

		if (norm)
		{
			UVWChannels[0] /= 0.66f;
			UVWChannels[2] /= 1.569149f;
		}

		merge(UVWChannels, output);
		break;
		//******************* YUV ****************
	case colorSpace::YUV:
		split(scaledImage, channels);
		YUVChannels[0] = 0.299f*channels[2] + 0.587f*channels[1] + 0.114f*channels[0];
		YUVChannels[1] = 0.492f*(channels[0]-YUVChannels[0]);
		YUVChannels[2] = 0.77f*(channels[2]-YUVChannels[0]);

		if (norm)
		{
			YUVChannels[1]  = (YUVChannels[1] + 0.435912f)/(0.871824f);
			YUVChannels[2]  = (YUVChannels[2] + 0.53977f)/(1.07954f);
		}

		merge(YUVChannels, output);
		break;

		//******************* OPP ****************
	case colorSpace::OPP:
		split(scaledImage, channels);
		OPPChannels[0] = (channels[2] - channels[1])/sqrt(2.f);
		OPPChannels[1] = (channels[2] + channels[1]-2.f*channels[0])/sqrt(6.f);
		OPPChannels[2] = (channels[0] + channels[1]+channels[2])/sqrt(3.f);

		if (norm)
		{
			float denom = 1.f/sqrt(2.f);
			float denom2 = 2.f/sqrt(6.f);
			OPPChannels[0] = (OPPChannels[0]+denom )/(denom*2.f);
			threshold(OPPChannels[0],OPPChannels[0], 0, 0,THRESH_TOZERO);
			OPPChannels[1] = (OPPChannels[1]+denom2 )/(denom2*2.f);
			threshold(OPPChannels[1],OPPChannels[1], 0, 0,THRESH_TOZERO);
			OPPChannels[2] /= 1.f/sqrt(3.f);
		}

		merge(OPPChannels, output);
		break;

		//******************* NOPP ****************
	case colorSpace::NOPP:
		split(scaledImage, channels);
		O3 = (channels[0] + channels[1] + channels[2])/sqrt(3.f);
		O1O2Channels[0] = (channels[2]-channels[1])/sqrt(2.f);
		divide(O1O2Channels[0], O3,O1O2Channels[0]);
		O1O2Channels[1] = (channels[2]+channels[1]-2.f*channels[0])/sqrt(6.f);
		divide(O1O2Channels[1], O3 ,O1O2Channels[1]);

		if (norm)
		{
			float denom1_1 = sqrt(3.f)/(sqrt(2.f));
			float denom1_2 = denom1_1*2.f;
			float denom2_1 = 2.f*sqrt(3.f)/sqrt(6.f);
			float denom2_2 =  denom2_1 + sqrt(3.f)/sqrt(6.f);

			O1O2Channels[0] = (O1O2Channels[0] + denom1_1) / denom1_2;
			threshold(O1O2Channels[0],O1O2Channels[0], 0, 0,THRESH_TOZERO);
			O1O2Channels[1] = (O1O2Channels[1] + denom2_1) / denom2_2;
			threshold(O1O2Channels[1],O1O2Channels[1], 0, 0,THRESH_TOZERO);
		}

		merge(O1O2Channels, output);
		break;

		//******************* xyY ****************
	case colorSpace::xyY:
		cvtColor(scaledImage , t , CV_BGR2XYZ);
		split(t, channels);
		divide(channels[0],channels[0]+channels[1]+channels[2], xyYChannels[0]);
		divide(channels[1],channels[0]+channels[1]+channels[2], xyYChannels[1]);
		xyYChannels[2] = channels[1];

		if(norm)
		{
			xyYChannels[0] = xyYChannels[0] / 0.639999814f ;
			xyYChannels[1] = xyYChannels[1] / 0.6f;
		}

		merge(xyYChannels, output);
		break;

		//******************* rg ****************
	case colorSpace::rg:
		split(scaledImage, channels);
		divide(channels[2],channels[0]+channels[1]+channels[2],rgChannels[0]);
		divide(channels[1],channels[0]+channels[1]+channels[2],rgChannels[1]);
		merge(rgChannels,output);
		break;

		//******************* YES ****************
	case colorSpace::YES:
		split(scaledImage, channels);
		YESChannels[0] = 0.253f*channels[2] + 0.684f*channels[1] + 0.063f*channels[0];
		YESChannels[1] = 0.5f*channels[2] - 0.5f*channels[1];
		YESChannels[2] = 0.250f*channels[2] + 0.250f*channels[1] - 0.5f*channels[0];
		if(norm)
		{
			YESChannels[1] += 0.5f;
			YESChannels[2] += 0.5f;
		}

		merge(YESChannels,output);
		break;

		//		//******************* TRGB ****************
		//	case colorSpace::TRGB:
		//		split(scaledImage, channels);
		//		//rgbmean = mean(scaledImage);
		//		meanStdDev(scaledImage, mean, dev);
		//		channels[0] = channels[0] - mean[0]/dev[0];
		//		channels[1] = channels[1] - mean[1]/dev[1];
		//		channels[2] = channels[2] - mean[2]/dev[2];
		//		merge(channels, output);
		//		break;


	case colorSpace::I1I2I3:
		split(scaledImage, channels);

		I3Channels[0] = (channels[0]+channels[1]+channels[2])/3.f;
		I3Channels[1] = (channels[2] - channels[0])/2.f;
		I3Channels[2] = (2*channels[1] - channels[2] -channels[0])/4.f;

		if(norm)
		{
			I3Channels[1] += 0.5f;
			I3Channels[2] += 0.5f;
		}

		merge(I3Channels, output);
		break;

	case colorSpace::RGB:
		output = scaledImage.clone();
		break;

	default:
		output = scaledImage.clone();
		break;

		return output;
	}
}
// Generates a ROS sensor message using an image
sensor_msgs::Image Saliency::fillImageMsgs(cv::Mat image, std::string imgName)
{
	cv_bridge::CvImage img_bridge;
	sensor_msgs::Image msg;
	std_msgs::Header header;
	header.frame_id = imgName;
	header.stamp = ros::Time::now();
	header.seq = counter;
	img_bridge = cv_bridge::CvImage(header, sensor_msgs::image_encodings::MONO8, image);
	img_bridge.toImageMsg(msg);
	counter++;
	return msg;
}
// Reads an image from a sensor message
cv::Mat Saliency::getImageFromMsg( sensor_msgs::Image msg)
{
	int rows = msg.height;
	int cols = msg.width;
	int step = 3;
	int type = -1;
	int size = rows*cols*step;
	int format = CV_8UC3;
	Mat img(rows, cols, format);
	memcpy(img.data, &msg.data[0], size);
	return img;
}


//****************************** Methods ******************************
// Generates a top-down saliency using backprojection. Image: input image, temp : object template.
// To set the parameters please refer to opencv documentation
cv::Mat Saliency::generateBackProjection(cv::Mat imageCV, cv::Mat temp)
{
	int channels [] = {0,1,2,3};
	int dim = imageCV.channels();
	int histSize[] = {num_bins,num_bins,num_bins,num_bins};
	float range[] = {0, 1.001};
	const float* ranges[] = {range, range ,range,range};

	Mat templateHistogram;

	calcHist(&temp,1,channels,Mat(),templateHistogram, dim, histSize, ranges, true, false);
	normalizeHistogram(templateHistogram);

	Mat backProjectedImage;
	calcBackProject(&imageCV, 1, channels, templateHistogram, backProjectedImage, ranges, 1, true );

	threshold(backProjectedImage, backProjectedImage, 0,255,THRESH_BINARY);
	backProjectedImage.convertTo(backProjectedImage, CV_8UC1);

	return backProjectedImage;

}
/* Load basis from a binary file
 * Expects the binary file to be formatted as follows:
 * first 3 floats are number of kernels, kernel_size
 * and number of channels in the image (1 for grayscale and 3 for rgb)
 * followed by the num_channels*num_kernels*kernel_size*kernel_size of floats
 * containing the basis written in a row-major order*/
void Saliency::loadBasis(std::string filename ) {
	FILE* kernel_file;
	kernel_file = fopen(filename.c_str(), "rb");
	float temp;

	fread(&temp, sizeof(float), 1, kernel_file); num_kernels = temp;
	fread(&temp, sizeof(float), 1, kernel_file); kernel_size = temp;
	fread(&temp, sizeof(float), 1, kernel_file); num_channels = temp;

	kernels = new Mat*[num_kernels];
	for (int i = 0; i < num_kernels; i++) {
		kernels[i] = new Mat[num_channels];
	}
	aim_temp = new Mat[num_kernels];
	data = new float[num_channels*num_kernels*kernel_size*kernel_size];

	//read all data into array of floats
	fread(data, sizeof(float), num_kernels*kernel_size*kernel_size*num_channels, kernel_file);
	int sizes[] = {kernel_size, kernel_size, num_channels};
	printf("Found %i kernels DIM %i x %i x %i\n", num_kernels, kernel_size, kernel_size, num_channels);
	printf("Loading kernels...\n");
	double min, max;
	//load data into Mat
	for (int c = 0; c < num_channels; c++) {
		for (int n = 0; n < num_kernels; n++) {
			kernels[n][c] = Mat(kernel_size, kernel_size, CV_32FC1, &data[c*num_kernels*kernel_size*kernel_size+n*kernel_size*kernel_size]);
			minMaxLoc(kernels[n][c], &min, &max);
		}
	}
	fclose(kernel_file);
}
cv::Mat Saliency::runAIM() {
	min_aim = 100000;
	max_aim = -1000000;

	//split image into channels
	split(image, channels);

	for (int c = 0; c < num_channels; c++) {
		channels[c].convertTo(channels[c], CV_32FC1);
		channels[c] /= 255.0f;
	}

	//apply all filters to each channel
	Point anchor(-1, -1);
	for (int f = 0; f < num_kernels; f++) {

		filter2D(channels[0], aim_temp[f], -1, kernels[f][0], anchor, 0, BORDER_CONSTANT);
		for(int c = 1; c < num_channels; c++) {
			filter2D(channels[c], temp, -1, kernels[f][c], anchor, 0, BORDER_CONSTANT);
			aim_temp[f] += temp;
		}
		//only keep the valid pixels after filtering
		aim_temp[f] = aim_temp[f].colRange((kernel_size)/2, image.cols - (kernel_size-1)/2)
						         .rowRange((kernel_size)/2, image.rows - (kernel_size-1)/2);
		minMaxLoc(aim_temp[f], &minVal, &maxVal);

		//compute max and min across all feature maps
		max_aim = fmax(maxVal, max_aim);
		min_aim = fmin(minVal, min_aim);
	}


	printf("Rescaling image ...\n");
	//rescale image using global max and min
	for (int f = 0; f < num_kernels; f++) {
		aim_temp[f] -= min_aim;
		aim_temp[f] /= (max_aim - min_aim);
	}

	//compute histograms for each feature map
	//and use them to rescale values based on histogram to reflect likelihood
	printf("Computing histograms for each feature ...\n");
	Mat hist;
	Mat sm = Mat(aim_temp[0].rows, aim_temp[0].cols, CV_32FC1, Scalar::all(0));;
	float div = (aim_temp[0].rows*aim_temp[0].cols);
	float histRange[] = {0, 1};
	const float *range[] = {histRange};
	int histSize[] = {256};
	for (int f = 0; f < num_kernels; f++) {
		calcHist(&aim_temp[f], 1, 0, Mat(), hist, 1, histSize, range, true, false);
		for(int i = 0; i < aim_temp[f].rows; i++) {
			for (int j = 0; j < aim_temp[f].cols; j++) {
				//find index of the value in the histogram
				int idx = round(aim_temp[f].at<float>(i, j) * (histSize[0]-1));
				//compute log probability
				sm.at<float>(i,j) -= log(hist.at<float>(idx)/div+0.000001f);
			}
		}
	}

	//find max and min of the final saliency map
	minMaxLoc(sm, &minVal, &maxVal);

	//rescale to [0, 255] for viewing
	sm.convertTo(adj_sm, CV_8UC1, 255/(maxVal-minVal), -minVal);
	//add blank border
	int border = kernel_size/2;
	copyMakeBorder(adj_sm, adj_sm, border, border, border, border, BORDER_CONSTANT, 0);

	//rescale image back to the original size
	resize(adj_sm, adj_sm, cvSize(0, 0), 1/scale , 1/scale);
	//imshow("SM", adj_sm);
	return adj_sm;
}
cv::Mat Saliency::generateAIMMap(cv::Mat imageName, float scale_factor,std::string basisName )
{
	image = imageName;
	scale = scale_factor;
	resize(image, image, cvSize(0, 0), scale, scale);

	if(!gotKernel){
		this->loadBasis(basisName);
		gotKernel = true;
	}

	return this->runAIM();
}

//****************************** Service Functions ******************************
bool Saliency::GetAIMMap(saliency::GetAIM::Request& req, saliency::GetAIM::Response& res)
{

	Mat imageInput = getImageFromMsg(req.input_image);
	Mat infoMap = generateAIMMap(imageInput, req.scale_factor, req.basis_name);
	Mat percInfoMap = percentileThreshold(infoMap, req.percentile);
	res.infomap = fillImageMsgs(percInfoMap, "AIMSaliency_p" + to_string(req.percentile));
	return true;
}
bool Saliency::GetBackProjMap(saliency::GetBackProj::Request& req, saliency::GetBackProj::Response& res)
{
	Mat tempImg = getImageFromMsg(req.template_image);
	Mat imageInput = getImageFromMsg(req.input_image);
	if (req.num_bins >= 0)
	{
		this->num_bins = req.num_bins;
	}
	tempImg = normalizeImage(tempImg);
	if (req.normalize)
	{
		imageInput = normalizeImage(imageInput);
	}
	std::vector<string>::iterator it;
	it = find (_colors.begin(), _colors.end(), req.color_space);
	int pos = it - _colors.begin();
	imageInput = imageConversion(imageInput, static_cast<colorSpace>(pos));
	tempImg  = imageConversion(tempImg , static_cast<colorSpace>(pos));
	Mat backprojImg = generateBackProjection(imageInput,tempImg);
	res.backproj_image = fillImageMsgs(backprojImg,"bpImg_c" + req.color_space + "_b" + to_string(this->num_bins));


	return true;
}

//****************************ROS Setup********************************

void Saliency::QueueThread() {
	static const double timeout = 0.01;
	while (this->rosNode->ok()) {
		this->rosQueue.callAvailable(ros::WallDuration(timeout));
		ros::spinOnce();
	}
}
void Saliency::ROSNodeInit()
{
	if (!ros::isInitialized()) {
		int argc = 0;
		char **argv = NULL;
		ros::init(argc, argv, namespace_,
				ros::init_options::NoSigintHandler);
	}
	this->rosNode.reset(new ros::NodeHandle(namespace_));
}
void Saliency::InitRosTopics()
{
	//***** Services ******
	this->getAIMSrv = this->rosNode->advertiseService(this->getAIMService , &Saliency::GetAIMMap, this);
	this->getBackProjSrv = this->rosNode->advertiseService(this->getBackProjService , &Saliency::GetBackProjMap, this);
	//this->callbackQueueThread = boost::thread(std::bind(&Saliency::QueueThread, this));
	ros::spinOnce();
}


int main(int argc, char **argv)
{
	ROS_INFO("Saliency service to generate AIM and Backprojection Maps!");

	Saliency sal;
	sal.ROSNodeInit();
	sal.InitRosTopics();

	while (ros::ok())
	{

		ros::spinOnce();

	}
	return 1;


}
