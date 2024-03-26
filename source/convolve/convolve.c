/**
    @file convolve - convolution between two buffers in Max
    @version 0.1.0
    @author isaiahdoyle - isaiahdoyle56@gmail.com
    @date Thu Apr 20 2023
*/

#include "ext.h"                    // standard Max include, always required
#include "ext_obex.h"               // required for new style Max object
#include "ext_buffer.h"             // for reading buffers

#include <math.h>
#include <Accelerate/Accelerate.h>  // includes vDSP functions for DFT (must be added as framework in XCode)

// object typedef, any attrs included here
typedef struct _convolve {
    t_object    ob;     // the object itself (must be first)
    void*       done;   // bang outlet
} t_convolve;

void *convolve_new(t_symbol *s, long argc, t_atom *argv);
void convolve_free(t_convolve *x);
void convolve_assist(t_convolve* x, void *b, long m, long a, char *s);
void convolve_defer(t_convolve* x, t_symbol* sym, short argc, t_atom* argv);
void convolve_main(t_convolve *x, t_symbol* sym, short argc, t_atom *argv);
void init_spectrum(t_convolve* x, DSPSplitComplex* spectrum, long fft_length, float* samples, long sig_length, short pack);
short get_log2(long n);
void write_little_endian(t_filehandle* file, int num_bytes, int word);
void write_wav(t_filehandle* file, unsigned long num_samples, float* data, int s_rate);

void *convolve_class; // global pointer to class for use by max

/* Max instantiation stuff */

C74_EXPORT void ext_main(void *r) {
    t_class *c;

    c = class_new(
                  "convolve",
                  (method)convolve_new,
                  (method)convolve_free,
                  sizeof(t_convolve),
                  0L,
                  A_GIMME,
                  0
                 );

    /* links convolve message to convolve_main() method */
    class_addmethod(c, (method)convolve_defer, "convolve", A_GIMME, 0);

    /* assistance messaging on inlets/outlets */
    class_addmethod(c, (method)convolve_assist, "assist", A_CANT, 0);

    class_register(CLASS_BOX, c);
    convolve_class = c;
}

void convolve_assist(t_convolve *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) { // inlet
        sprintf(s, "(message): convolve output_buffer IR_buffer signal_buffer");
    }
    else { // outlet
        sprintf(s, "bang on success");
    }
}

void convolve_free(t_convolve *x) {
    ;
}

void *convolve_new(t_symbol *s, long argc, t_atom *argv) {
    t_convolve *x = NULL;
    long i;

    x = (t_convolve *)object_alloc(convolve_class);
    x->done = bangout((t_object*)x);

    return x;
}

/* main convolve methods */

void convolve_defer(t_convolve* x, t_symbol* sym, short argc, t_atom* argv) {
    /* writing to files requires deferring */
    defer(x, (method)convolve_main, sym, argc, argv);
}

void convolve_main(t_convolve* x, t_symbol* sym, short argc, t_atom* argv) {
    if (argc < 2) {
        object_error((t_object*)x, "usage: (convolve buffin1, buffin2)");
        return;
    }

    /* gather data from buffers */
    t_buffer_ref* ref_buffin1 = buffer_ref_new((t_object *)x, atom_getsym(argv++));
    t_buffer_ref* ref_buffin2 = buffer_ref_new((t_object *)x, atom_getsym(argv++));
    t_buffer_obj* buffin1 = buffer_ref_getobject(ref_buffin1);
    t_buffer_obj* buffin2 = buffer_ref_getobject(ref_buffin2);

    t_atom_long framecount1 = buffer_getframecount(buffin1); // # samples
    t_atom_long framecount2 = buffer_getframecount(buffin2);
    t_atom_float sr1 = buffer_getsamplerate(buffin1);
    t_atom_float sr2 = buffer_getsamplerate(buffin2);

    if (buffer_getchannelcount(buffin1) || buffer_getchannelcount(buffin2)) {

    }

    if (framecount1 < 8 || framecount2 < 8) {
        object_error((t_object*)x, "at least one input buffer is too short");
        return;
    } else if (buffer_getchannelcount(buffin1) > 1 || buffer_getchannelcount(buffin2) > 1) {
        object_warn((t_object*)x, "this object doesn't support non-mono signals... your output will probably be stretched!");
    } else if (sr1 != sr2) {
        object_warn((t_object*)x, "input buffers have varying sample rates");
    }

    /* prepare output file */
    t_fourcc filetype='WAVE', outtype;
    short numtypes = 1;
    char filename[MAX_FILENAME_CHARS];
    short path;
    if (saveasdialog_extended(filename, &path, &outtype, &filetype, 1)) return;

    /* retrieve input samples */
    float* samples1 = buffer_locksamples(buffin1);
    float* samples2 = buffer_locksamples(buffin2);

    /* length of the signal after convolution is length1 + length2 - 1 */
    long fft_length = framecount1 + framecount2 - 1;

    /* set fft_length next highest power of 2 */
    short log2n = get_log2(fft_length);
    fft_length = 1U << log2n;

    /* find spectrums of both signals */
    DSPSplitComplex spectrum1;  // input 1
    DSPSplitComplex spectrum2;  // input 2
    DSPSplitComplex spectrum;   // output
    init_spectrum(x, &spectrum1, fft_length, samples1, framecount1, 1);
    init_spectrum(x, &spectrum2, fft_length, samples2, framecount2, 1);
    init_spectrum(x, &spectrum,  fft_length, NULL,     NULL,        0);

    /* pre-compute FFT bins */
    FFTSetup setup = vDSP_create_fftsetup(log2n, FFT_RADIX2);

    if (!setup) {
        object_error((t_object *) x, "could not pre-compute FFT bins");
        return;
    }

    /* compute FFT */
    vDSP_fft_zrip(setup, &spectrum1, 1, get_log2(framecount1), kFFTDirection_Forward);
    vDSP_fft_zrip(setup, &spectrum2, 1, get_log2(framecount2), kFFTDirection_Forward);

    /* data packing is weird. this preserves nyquist bin for spectrum multiplication */
    float nyq1 = spectrum1.imagp[0];
    float nyq2 = spectrum2.imagp[0];
    spectrum1.imagp[0] = 0;
    spectrum2.imagp[0] = 0;

    /* multiply both spectrums (time-domain convolution) */
    vDSP_zvmul(&spectrum1, 1, &spectrum2, 1, &spectrum, 1, fft_length/2, 1);

    spectrum.imagp[0] = nyq1 * nyq2;

    /* inverse DFT result to time-domain (convoluted signal stored in spectrum) */
    vDSP_fft_zrip(setup, &spectrum, 1, log2f(framecount1 + framecount2 - 1), kFFTDirection_Inverse);

    /* unpack to output buffer */
    float* samples = (float*)malloc(sizeof(float)*fft_length/2);
    vDSP_ztoc(&spectrum, 1, (DSPComplex*)samples, 2, (long)fft_length/4);

    /* normalization */
    float scale = 1.f/samples[0];
    vDSP_vsmul(samples, 1, &scale, samples, 1, fft_length/2);

    /* write to .WAV file */
    t_filehandle file;
    if (path_createsysfile(filename, path, 'WAVE', &file)) {
        object_error((t_object*)x, "could not create output file");
        return;
    }

    write_wav(&file, fft_length/2, samples, sr1);

    /* free memory | unclaim buffers */
    vDSP_destroy_fftsetup(setup);
    free(samples);
    free(spectrum.imagp);
    free(spectrum.realp);
    free(spectrum2.imagp);
    free(spectrum2.realp);
    free(spectrum1.imagp);
    free(spectrum1.realp);
    buffer_unlocksamples(buffin2);
    buffer_unlocksamples(buffin1);

    /* bang! */
    outlet_bang(x->done);
}

/**
 @method `init_spectrum`
 allocate spectrum memory, and pack samples into `DSPSplitComplex` format if `pack` is set

 - Parameters:
    - x: object
    - samples: samples to pack (only if `pack` is set)
    - spectrum: the spectrum to initialize
    - sig_length: length of the signal (only if `pack` is set)
    - fft_length: length of the fft
    - pack: `1` to pack values into even-odd split format, `0` otherwise
*/
void init_spectrum(t_convolve* x, DSPSplitComplex* spectrum, long fft_length, float* samples, long sig_length, short pack) {
    /* this should be freed at some point the caller method */
    spectrum->realp = (float*)malloc(sizeof(float)*fft_length/2);
    spectrum->imagp = (float*)malloc(sizeof(float)*fft_length/2);

    if (!spectrum->realp || !spectrum->imagp) {
        object_error((t_object*)x, "could not allocate memory for spectrums");
        return;
    }

    /* vDSP data packing requires that the samples be stored as complex numbers
       (e.g., [1, 2, 3, 4, ...] -> [(1 + j2), (3 + j4), ...]) */
    if (pack) {
        vDSP_ctoz((DSPComplex*)samples, 2, spectrum, 1, (long)sig_length/2);

        /* pad remaining samples with zeroes */
        for (uintptr_t i = sig_length/2; i < fft_length/2; i++) {
            spectrum->realp[i] = 0.0;
            spectrum->imagp[i] = 0.0;
        }
    }
}

/**
 @method `get_log2n`
 rounds n to the next highest power of 2, and returns the base 2 log of that number
 (in other words, returns how many bits are necessary to store n)

 - Parameter n: any number
*/
short get_log2(long n) {
    short log2n = 0;

    while (n) {
        n >>= 1;
        log2n++;
    }

    return log2n;
}

/**
 the following two methods `write_little_endian` and `write_wav` are both slightly modified
 versions of Kevin Karplus' methods from `make_wav.c` to support Max formatting. check out his blog!
    https://gasstationwithoutpumps.wordpress.com/2011/10/08/making-wav-files-from-c-programs/

 copyright
 Fri Jun 18 16:36:23 PDT 2010 Kevin Karplus
 Creative Commons license Attribution-NonCommercial
    http://creativecommons.org/licenses/by-nc/3.0/
*/
void write_little_endian(t_filehandle* file, int num_bytes, int word) {
    t_ptr_size ptr = 1;
    int buf;

    while(num_bytes>0) {
        buf = word & 0xff;
        sysfile_write(*file, &ptr, &buf);
        num_bytes--;
        word >>= 8;
    }
}

void write_wav(t_filehandle* file, unsigned long num_samples, float * data, int s_rate) {
    unsigned int sample_rate;
    unsigned int num_channels;
    unsigned int bytes_per_sample;
    unsigned int byte_rate;
    unsigned long i; // counter for samples

    num_channels = 1;
    bytes_per_sample = 2;

    /* sysfile_write asks for the number of bytes to be a pointer
       so it can overwrite with the actual number of bytes written */
    t_ptr_size ptr2 = 2;
    t_ptr_size ptr4 = 4;

    if (s_rate<=0) sample_rate = 44100;
    else sample_rate = (unsigned int) s_rate;

    byte_rate = sample_rate*num_channels*bytes_per_sample;

    /* write RIFF header */
    sysfile_write(*file, &ptr4, "RIFF");                                                // chunk id
    write_little_endian(file, 4, (int)(36+bytes_per_sample*num_samples*num_channels));  // chunk size
    sysfile_write(*file, &ptr4, "WAVE");                                                // chunk format

    /* write fmt subchunk */
    sysfile_write(*file, &ptr4, "fmt ");                                                // subchunk id (fmt)
    write_little_endian(file, 4, 16);                                                   // subchunk size (16)
    write_little_endian(file, 2, 1);                                                    // audio format (uncompressed)
    write_little_endian(file, 2, num_channels);                                         // # channels
    write_little_endian(file, 4, sample_rate);                                          // sample rate
    write_little_endian(file, 4, byte_rate);                                            // byte rate (bytes/second)
    write_little_endian(file, 2, num_channels*bytes_per_sample);                        // block alignment
    write_little_endian(file, 2, 8*bytes_per_sample);                                   // bits per sample

    /* write data subchunk */
    sysfile_write(*file, &ptr4, "data");                                                // subchunk id (data)
    write_little_endian(file, 4, (int)(bytes_per_sample*num_samples*num_channels));     // subchunk size (data length)
    for (i = 0; i < num_samples; i++) {
        write_little_endian(file, bytes_per_sample, (int)(data[i]*255));                // samples written here
    }

    sysfile_close(*file);
}
