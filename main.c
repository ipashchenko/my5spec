/***************************************************************************
 *   Copyright (C) 2015 by Petr Voytsik                                    *
 *                                                                         *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <complex.h>
#include <fftw3.h>
#include <mark5access.h>


struct fft_data_t {
    fftwf_plan *plan;
    fftwf_complex **zdata;
    float **data;
    double **spec;
    unsigned n_sp_chann;
    unsigned n_if;
};

static int fft_data_init(struct fft_data_t *fft_data, unsigned n_sp_chann, unsigned n_if)
{
    unsigned i;
    int ret = 0;

    fft_data->n_if = n_if;
    fft_data->n_sp_chann = n_sp_chann;
    fft_data->spec = (double **)malloc(n_if * sizeof(double *));
    fft_data->data = (float **)malloc(n_if * sizeof(float *));
    fft_data->zdata = (fftwf_complex **)malloc(n_if * sizeof(fftwf_complex *));
    fft_data->plan = (fftwf_plan *)malloc(n_if * sizeof(fftwf_plan));

    for(i = 0; i < n_if; ++i){
        fft_data->spec[i] = (double *)malloc(n_sp_chann * sizeof(double));
        fft_data->zdata[i] = fftwf_alloc_complex(n_sp_chann + 2);
        /* data[i] = (float *)calloc(2*nchan+2, sizeof(float)); */
        /* Use in-place FFT */
        fft_data->data[i] = (float *)(fft_data->zdata[i]);
        fft_data->plan[i] = fftwf_plan_dft_r2c_1d(n_sp_chann * 2, fft_data->data[i], 
                                                  fft_data->zdata[i], FFTW_MEASURE);
    }

    return ret;
}

static void fft_data_free(struct fft_data_t *fft_data)
{
    unsigned i;

    for(i = 0; i < fft_data->n_if; ++i){
        fftwf_destroy_plan(fft_data->plan[i]);
        free(fft_data->spec[i]);
        fftwf_free(fft_data->zdata[i]);
    }
    fftwf_cleanup();
    free(fft_data->plan);
    free(fft_data->spec);
    free(fft_data->data);
    free(fft_data->zdata);

}

/*
static int mark5_stream_set_time_offset(struct mark5_stream *ms, 
                                        double offset)
{
    int mjd, sec, ret = 0;
    double ns;

    if(!ms){
        return -1;
    }

    ret = mark5_stream_get_sample_time(ms, &mjd, &sec, &ns);
    if(ret == -1){
        fprintf(stderr, "Error in mark5_stream_get_sample_time\n");

        return ret;
    }

    return 0;
}
*/

static int spec(const char *input_filename, const char *format, 
                int nchan, double aver_time, double total_time, double offset,
                const char *out_filename_base)
{
    struct mark5_stream *ms;
    struct fft_data_t fft_data;
    int nint;
    double real_step;
    int i, j, k;
    int c;  /* Iteration over spectral channels */
    int step_num = 0;  /* Number of time steps */
    FILE **out_files;
    char *out_filename;

    ms = new_mark5_stream_absorb(new_mark5_stream_file(input_filename, 0LL),
                                 new_mark5_format_generic_from_string(format));
    if(!ms){
        fprintf(stderr, "Error: problem opening %s\n", input_filename);

        return EXIT_FAILURE;
    }

    /* Prepare output files */
    out_filename = (char *)malloc(strlen(out_filename_base) * sizeof(char) + 4);
    out_files = (FILE **)malloc(ms->nchan * sizeof(FILE *));
    for(i = 0; i < ms->nchan; ++i){
        sprintf(out_filename, "%s_%02d", out_filename_base, i+1);
        out_files[i] = fopen(out_filename, "w");
        if(!out_files[i]){
            perror("Could not open output file");

            return EXIT_FAILURE;
        }
    }
    free(out_filename);

    nint = aver_time * ms->samprate / (2 * nchan);
    printf("nint = %d\n", nint);
    real_step = (double)(nint * 2 * nchan) / (double)ms->samprate;
    printf("Real time step = %lf ms\n", real_step * 1e3);
    step_num = (int)(total_time / real_step);
    printf("Number of time steps = %d\n", step_num);

    /* Prepare data arrays */
    fft_data_init(&fft_data, nchan, ms->nchan);

    for(k = 0; k < step_num; ++k){
        /* Zero spec */
        for(i = 0; i < ms->nchan; ++i)
            for(c = 0; c < nchan; ++c)
                fft_data.spec[i][c] = 0.0;

        for(j = 0; j < nint; ++j){
            int status;
            double re, im;
            
            status = mark5_stream_decode(ms, 2*nchan, fft_data.data);
            if(status < 0){
                fprintf(stderr, "Error: mark5_stream_decode failed\n");
                break;
            }
            if(ms->consecutivefails > 5){
                fprintf(stderr, "Error: problem with data decoding\n");
                break;
            }
            for(i = 0; i < ms->nchan; ++i){
                fftwf_execute(fft_data.plan[i]);
                for(c = 0; c < nchan; ++c){

                    re = creal(fft_data.zdata[i][c]);
                    im = cimag(fft_data.zdata[i][c]);
                    fft_data.spec[i][c] += (re*re + im*im) / (double)(2*nchan);
                }
            }
        }

        for(i = 0; i < ms->nchan; i++){
            for(c = 0; c < nchan; ++c){
                fprintf(out_files[i], "%lf ", fft_data.spec[i][c] / (double)nint);
            }
            fputc('\n', out_files[i]);
        }

    }

    /* Free resources */
    for(i = 0; i < ms->nchan; ++i)
        fclose(out_files[i]);
    free(out_files);
    fft_data_free(&fft_data);
    delete_mark5_stream(ms);

    return EXIT_SUCCESS;
}

static void usage(const char *prog_name)
{
    printf("Usage: %s [-a aver_time] [-n nchan] [-l time_limit] [-o offset] INFILE FORMAT OUTFILE \n\n", 
            prog_name);
    printf("INFILE    - the name of the input file\n");
    printf("FORMAT    - mark5access data format in form <FORMAT>-<Mbps>-<nchan>-<nbit>\n");
    printf("OUTFILE   - basename for the output files.\n\
Output files will be called 'OUTFILE_n', where n is IF number\n");
    printf("\noptional arguments:\n");
    printf("  -a aver_time  - approximate integration time per spectrum in milliseconds (1 ms)\n");
    printf("  -n nchan      - number of spectral channels (128)\n");
    printf("  -l time_limit - total time in seconds\n");
    printf("  -o offset     - offset in seconds from the file beginnig (0)\n");
}

int main(int argc, char *argv[])
{
    int ret = 0, opt;

    /* Parameters */
    int nchan = 128;
    double aver_time = 1e-3;    /* 1ms */
    double total_time = 1200;   /* 20 min */
    double offset = 0.0;        /* No offset */

    /* Check input parameters */
    while((opt = getopt(argc, argv, "n:a:l:o:h")) != -1){
        switch(opt){
            case 'n':
                nchan = atoi(optarg);
                if(nchan <=0 || nchan > 2<<15){
                    fprintf(stderr, "Ivalid number of spectral channels: %d\n", nchan);

                    exit(EXIT_FAILURE);
                }
                break;
            case 'a':
                aver_time = atof(optarg);
                if(aver_time <= 0){
                    fprintf(stderr, "Invalid average time: %lf\n", aver_time);

                    exit(EXIT_FAILURE);
                }
                aver_time *= 1e-3;
                break;
            case 'l':
                total_time = atof(optarg);
                if(total_time <= 0){
                    fprintf(stderr, "Invalid time limit: %lf\n", total_time);

                    exit(EXIT_FAILURE);
                }
                break;
            case 'o':
                offset = atof(optarg);
                if(offset < 0){
                    fprintf(stderr, "Negative offset is not supported\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'h':
                usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if(optind >= argc - 2){
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    ret = spec(argv[optind], argv[optind+1], nchan, aver_time, total_time, offset,
               argv[optind+2]);

    return ret;
}
