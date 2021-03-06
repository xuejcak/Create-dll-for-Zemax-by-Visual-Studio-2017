#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cmath>
#include <iostream>
#include <Windows.h>
#include <iomanip>
#include <map>
#include <random>

#pragma warning ( disable : 4996 ) // functions like strcpy are now deprecated for security reasons

/*
Written by Sanjay Gangadhara and Kenneth Moore
September 15, 2008

Fixed typo in Importance Sampling algorithm
May 27, 2009  SG

Fixed scattering algorithm for non-normal incidence
March 10, 2010  SG

In importance sampling, BSDF*cos(theta) is returned but not BSDF.
Only importance sampling should be used. If not, since scatter angle is distributed uniformly,
a few rays will hit the next surface.
December 10, 2018 Jianchao Xue
*/

//Questions about K-correlation.dll
#define CRM_CASE_6752 1 

extern "C" {
	int __declspec(dllexport) APIENTRY UserScatterDefinition(double *data);
	int __declspec(dllexport) APIENTRY UserParamNames(char *data);
}
void CrossProduct(double x1, double y1, double z1, double x2, double y2, double z2, double *x3, double *y3, double *z3);
void Normalize(double *x, double *y, double *z);

BOOL WINAPI DllMain(HANDLE hInst, ULONG ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

/* the data is stored as follows:
data[ 0]  = the total number of (double) values in the passed data array
data[ 1]  = x position of specular ray
data[ 2]  = y position of specular ray
data[ 3]  = z position of specular ray
data[ 4]  = x cosine of specular ray, on output it is the scattered ray
data[ 5]  = y cosine of specular ray, on output it is the scattered ray
data[ 6]  = z cosine of specular ray, on output it is the scattered ray
data[ 7]  = x normal
data[ 8]  = y normal
data[ 9]  = z normal

data[10] = 0 initially
If the DLL scatters the ray return 1 in data[10].
If the DLL returns full polarization data return 2 in data[10].

data[11] = millimeters per unit length (1.0 for mm, 25.4 for inches, 10.0 for cm and 1000.0 for meters)
data[12] = relative energy (to be computed by the dll and returned)
data[13] = incident media index
data[14] = substrate media index
data[15] = 0 for reflection, 1 for refraction, 2 for scatter function viewer
data[16] = a random value to use as a seed
data[17] = wavelength in 祄

// The following feature is used only for importance sampling
data[18] = 0 normally

If data[18] = -1, ZEMAX is requesting the DLL compute the total integrated scatter
instead of the scattered ray vector. Once the TIS is computed, return the TIS in data[18].
If the DLL cannot compute the TIS and BSDF, ignore data[18] and leave the value unchanged.
ZEMAX will then call the usual scatter algorithm and not use importance sampling.

If data[18] = -2, ZEMAX is requesting the DLL compute the BSDF instead of the scattered
ray vector. Once the BSDF is computed, return the BSDF in data[18]. If the DLL cannot
compute the BSDF, ignore data[18] and leave the value unchanged. ZEMAX will then call
the usual scatter algorithm and not use importance sampling.

The BSDF in general depends upon the specular ray and the scattered ray ZEMAX has chosen to trace.
The scattered ray ZEMAX has already chosen is stored in data[30] - data[32] below.


data[20] = incident Ex real
data[21] = incident Ex imaginary
data[22] = incident Ey real
data[23] = incident Ey imaginary
data[24] = incident Ez real
data[25] = incident Ez imaginary

The following feature is used only for importance sampling, see discussion above
data[30] = scattered ray x cosine
data[31] = scattered ray y cosine
data[32] = scattered ray z cosine

Data 40-45 need to be computed if the DLL sets data[10] = 2
data[40] = output Ex real
data[41] = output Ex imaginary
data[42] = output Ey real
data[43] = output Ey imaginary
data[44] = output Ez real
data[45] = output Ez imaginary

data[50] = The maximum number of parameters passed
data[51] = input parameter 1 from user
data[52] = input parameter 2 from user
etc... up to data[50 + maxdata] where maxdata = int(data[50])

data[200] - data[249] = reserved block of data (400 bytes) for the data string argument
data[250] - data[299] = reserved block of data (400 bytes) for the suggested path for the DLL data

Return 0 if it works; else return -1.

*/

/* This DLL models a surface that scatters according to the K-correlation distribution */

int __declspec(dllexport) APIENTRY UserScatterDefinition(double *data)
{
	int n_iter;
	double nx, ny, nz, sx, sy, sz, px, py, pz, qx, qy, qz;
	double ref_wav, delta_n, sigma, B, S, wave, inc_ang;
	double bo, MAG, T1, TIS, a, a1, c, sig_num, sig_den;
	double random_number, random_val1, random_val2, ray_test;
	double rsc, old_rsc, roff, rad, phi, bp, bq, xp, xq, xsq;
	double bx, by, bz, theta, beta, bsdf;
	const double PI = 4.0*atan(1.0);

	/* We need some way of randomizing the random number generator */
	std::random_device rd;
	std::mt19937 gen((unsigned int)data[16]);
	std::uniform_real_distribution<> dist(0.0, 1.0);

	/*
	The main thing is to make sure the scattered ray lies within a unit circle centered on the
	projected specular vector. The angle between the normal and the scattered ray must also be
	less than 90 degrees. We check this before returning, see below.
	*/

	sx = data[4];	// Specular ray
	sy = data[5];
	sz = data[6];

	nx = data[7];	// Normal vector
	ny = data[8];
	nz = data[9];

	/* Find vectors p,q perpendicular to n, such that the specular ray lies along the p-n plane */
	px = sx; 	    // Initialize p with values from the specular ray
	py = sy;
	pz = sz;

	MAG = px * nx + py * ny + pz * nz;

	if (MAG > 0.9999999)
	{
		/* specular is very close to the normal, any p will do */
		if (fabs(nz) < 0.9)
		{
			px = 0.0;
			py = 0.0;
			pz = 1.0;
		}
		else
		{
			px = 1.0;
			py = 0.0;
			pz = 0.0;
		}
	}

	/* This creates q normal to n and p */
	CrossProduct(nx, ny, nz, px, py, pz, &qx, &qy, &qz);
	Normalize(&qx, &qy, &qz);
	/* This creates p normal to both q and n */
	CrossProduct(nx, ny, nz, qx, qy, qz, &px, &py, &pz);
	//Normalize(&px, &py, &pz);	// Not needed since n and q are orthonormal already

	/* The specular ray, when projected onto the surface, lies along p */
	bo = px * sx + py * sy + pz * sz;

	/* Calculate the incident angle */
	inc_ang = acos(MAG);		// Incident angle in radians
	if (inc_ang > 0.5*PI) inc_ang = PI - inc_ang;

	/* Retrieve values for the model parameters */
	sigma = data[51];			// RMS surface roughness (microns)
	ref_wav = data[52];			// Reference wavelength (microns)
	B = data[53];				// 2*pi*L; L = typical surface wavelength (microns)
	S = data[54];				// Slope of BSDF at large spatial frequencies

	/* Calculate the change in index across the surface boundary */
	delta_n = 0.0;
	if (data[15] == 0.0)
	{
		delta_n = 2.0*data[13];					// Scattering in reflection
	}
	if (data[15] == 1.0)
	{
		delta_n = data[14] - data[13];			// Scattering in transmission
	}
	if (data[15] == 2)
	{
		delta_n = data[55];						// Scatter Function Viewer
	}

	/* Make sure that the parameters are reasonable */
	if (sigma <= 0.0) return -1;
	if (ref_wav <= 0.0) return -1;
	if (B <= 0.0) return -1;
	if (S <= 0.0) return -1;
	if (delta_n == 0.0) return -1;

	/* Calculate fixed constants based on model parameters, input wavelength */
	wave = data[17];
	a = (B*B) / (wave*wave);
	c = 0.5*(S - 2.0);

	/* Calculate the RMS surface roughness at the input wavelength */
	a1 = (B*B) / (ref_wav*ref_wav);
	if (S == 2)
	{
		sig_num = log(1 + a);
		sig_den = log(1 + a1);
	}
	else
	{
		sig_num = 1.0 - pow(1.0 + a, -1.0*c);
		sig_den = 1.0 - pow(1.0 + a1, -1.0*c);
	}
	sigma = sigma * sqrt(sig_num / sig_den);

	/* Calculate the simplified TIS at an incident angle = 0.0. If TIS > 1, don't scatter */

	T1 = 2 * PI*delta_n*sigma / wave;
	TIS = pow(T1, 2.0); //This is TIS at normal incidence
	if (TIS > 1.0) return -1;

	/* The following code is only used to implement importance sampling */
	if (data[18] < 0)
	{
		if (data[18] == -1)
		{
			// Compute the TIS and return the value in data[18]
			// The TIS value is calculated above
			data[18] = TIS;
#if CRM_CASE_6752
			double cos_i = cos(inc_ang);
			data[18] *= cos_i * cos_i;
#endif
			return 0;
		}
		if (data[18] == -2)
		{
			// Compute the BSDF for the ray ZEMAX has chosen and 
			// return the value in data[18]
			theta = acos(data[30] * nx + data[31] * ny + data[32] * nz);
			if (theta > 0.5*PI) theta = PI - theta;
			bp = data[30] * px + data[31] * py + data[32] * pz;
			bq = data[30] * qx + data[31] * qy + data[32] * qz;
			xsq = (bp - bo)*(bp - bo) + bq * bq;
			if (S == 2)
			{
				// BSDF*cos(theta) is returned but not BSDF, changed by Xue
				// Refer to Lambertian.c
				data[18] = TIS * a*cos(inc_ang)*cos(theta) / (PI*log(1.0 + a)*(1.0 + a * xsq))*cos(theta);
			}
			else
			{
				data[18] = TIS * a*cos(inc_ang)*cos(theta)*c / (PI*(1.0 - pow(1.0 + a, -1.0*c))*pow(1.0 + a * xsq, 0.5*S))*cos(theta);
			}
			return 0;
		}
		return 0;
	}

	/* If importance sampling is not used, use TIS to determine scatter fraction and
	   if appropriate return flag to indicate that we scattered */
	random_number = dist(gen);
#if CRM_CASE_6752
	double cos_i = cos(inc_ang);
	if (random_number > TIS*cos_i*cos_i) return -1;
#else	
	if (random_number > TIS) return -1;
#endif
	data[10] = 1.0;

	/* Get the scattered ray */
get_another_scattered_ray:;

	ray_test = 0.0;
	n_iter = 0;
	rsc = 1.0;
	roff = 0.0;
	while (ray_test < 1.0)
	{
		old_rsc = rsc;
		if (++n_iter > 100) rsc *= 0.99;
		if (++n_iter > 200) rsc *= 0.90;
		if ((old_rsc != rsc) && (fabs(bo) > 1.0e-8))
		{
			if (fabs(roff) < fabs(bo))
			{
				roff = (1.0 - rsc)*bo / fabs(bo);
			}
			else
			{
				roff = bo;
			}
		}
		random_val1 = dist(gen);
		random_val2 = dist(gen);
		rad = rsc * sqrt(random_val1);	// Magnitude of projected beta vector (maximum value = 1)
		phi = 2.0*PI*random_val2;		// Azimuthal angle for beta vector in projected surface
		bp = rad * sin(phi) + roff;		// Coordinates of beta vector on projected surface 
		bq = rad * cos(phi);
		xp = bp - bo;					// Coordinates of x vector on projected surface
		xq = bq;
		xsq = xp * xp + xq * xq;
		bx = bp * px + bq * qx;				// b is the total projected scattered ray vector
		by = bp * py + bq * qy;
		bz = bp * pz + bq * qz;
		MAG = bx * bx + by * by + bz * bz;
		beta = sqrt(MAG);
		theta = asin(beta);				// Scatter angle (radians)
		if (theta > 0.5*PI) theta = PI - theta;
		bsdf = cos(theta) / pow(1.0 + a * xsq, 0.5*S);
		random_number = dist(gen);
		if ((random_number < bsdf) && (MAG <= 1.0)) ray_test = 1.0;
	}
	MAG = sqrt(1.0 - MAG);

	/* This is the scattered ray */
	data[4] = bx + MAG * nx;
	data[5] = by + MAG * ny;
	data[6] = bz + MAG * nz;

	/* Return transmission */
#if CRM_CASE_6752
	data[12] = 1.0;
#else
	data[12] = cos(inc_ang);
#endif

	/*
	NOW, make sure we didn't scatter in such a way that the ray is now more than 90 degrees from normal!
	*/

	if (nx*data[4] + ny * data[5] + nz * data[6] < 0.0)
	{
		goto get_another_scattered_ray;
	}

	return 0;
}

int __declspec(dllexport) APIENTRY UserParamNames(char *data)
{
	/* this function returns the name of the parameter requested */
	int i;
	i = (int)data[0];
	strcpy(data, "");
	if (i == 1) strcpy(data, "Sigma");
	if (i == 2) strcpy(data, "Ref. Wave.");
	if (i == 3) strcpy(data, "B");
	if (i == 4) strcpy(data, "S");
	if (i == 5) strcpy(data, "SFV1");
	return 0;
}

void CrossProduct(double x1, double y1, double z1, double x2, double y2, double z2, double *x3, double *y3, double *z3)
{
	*x3 = y1 * z2 - z1 * y2;
	*y3 = z1 * x2 - x1 * z2;
	*z3 = x1 * y2 - y1 * x2;
}

void Normalize(double *x, double *y, double *z)
{
	double temp;
	temp = (*x)*(*x) + (*y)*(*y) + (*z)*(*z);
	temp = sqrt(temp);
	if (temp == 0) return;
	temp = 1.0 / temp;
	*x *= temp;
	*y *= temp;
	*z *= temp;
}


