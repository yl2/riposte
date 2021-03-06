
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <algorithm>

#include "Dense"

using Eigen::MatrixXd;
using Eigen::Map;

#include<iostream>

#include <sys/time.h>

#define R 100000
#define C 50

double current_time() {
	struct timeval v;
	gettimeofday(&v, NULL);
	return v.tv_sec +  v.tv_usec / 1000000.0;
}

int main() {
	double (*data)[C] = (double (*) [C]) malloc(R * C * sizeof(double));
	FILE * file = fopen("../pca.txt","r");
	assert(file);
	
	for(int c = 0; c < C; c++) {
		for(int r = 0; r < R; r++) {
			fscanf(file,"%lf",&data[r][c]);
		}
	}
	
	fclose(file);
	
	double begin = current_time();
	double * means = new double[C];
	std::fill(means,means+R,0.0);
	for(int r = 0; r < R; r++) {
		for(int c = 0; c < C; c++) {
			means[c] += (data[r][c] - means[c])/(r+1);
		}
	}
	//for(int c = 0; c < C; c++)
	//  printf("%f\n",means[c]);
	
	double (*cov)[C] = (double (*) [C]) malloc(C * C * sizeof(double));
	bzero(cov,sizeof(double) * C * C);
	
	for(int r = 0; r < R; r++) {
		for(int c0 = 0; c0 < C; c0++) {
			for(int c1 = c0; c1 < C; c1++) {
				double v  = (data[r][c0] - means[c0]) * (data[r][c1] - means[c1]);
				cov[c0][c1] += v;
				if(c0 != c1)
					cov[c1][c0] += v;
			}
		}
	}
	
	for(int c0 = 0; c0 < C; c0++) {
		for(int c1 = 0; c1 < C; c1++) {
			cov[c0][c1] /= (R - 1);
		}
	}
	printf("cov %f\n",current_time() - begin);
	MatrixXd cov_m = Map<MatrixXd>((double*)cov, C, C);
	
	
	//std::cout << cov_m << std::endl;
	
	double (*eig)[C] = (double (*)[C]) malloc(C * C * sizeof(double));
	
	MatrixXd eig_m  = Map<MatrixXd>((double*)eig, C, C);
	
	Eigen::SelfAdjointEigenSolver<MatrixXd> eigensolver(cov_m);
	printf("begin\n");
	eig_m = eigensolver.eigenvectors();
	
	double end = current_time();
	
	std::cout << eig_m(0,0) << std::endl;
	printf("%f\n",end - begin);
	//std::cout << eig_m << std::endl;
	
	
	double (*result)[C] = (double (*)[C]) malloc(R * C * sizeof(double));
	
	
	
	MatrixXd data_m = Map<MatrixXd>((double*)data, R, C);
	Map<MatrixXd>((double*)result, R, C) = data_m * eig_m;	
	
	return 0;
}