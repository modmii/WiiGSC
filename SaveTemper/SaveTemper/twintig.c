//Copyright 2007-2009  Segher Boessenkool  <segher@kernel.crashing.org>
// Licensed under the terms of the GNU GPL, version 2
// http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "tools.h"

static int verbose = 0;

#define MAXFILES 1000

#define ERROR(s) do { fprintf(stderr, s "\n"); exit(1); } while (0)

static u8 sd_key[16];
static u8 sd_iv[16];
static u8 md5_blanker[16];

static u32 ng_id;
static u32 ng_key_id;
static u8 ng_mac[6];
static u8 ng_priv[30];
static u8 ng_sig[60];

static FILE *fp;

static u8 header[0xf0c0];

static u32 n_files;
static u32 files_size;

static u8 files[MAXFILES][0x80];

static int read_image(u8 *data, u32 w, u32 h, const char *name)
{
	FILE *fp;
	u32 x, y;
	u32 ww, hh;

	fp = fopen(name, "rb");
	if (!fp) {
		fatal(-1,"open %s", name);
		fclose(fp);
		return -1;
	}

	if (fscanf(fp, "P6 %d %d 255", &ww, &hh) != 2) {
		fatal(-1,"bad ppm");
		fclose(fp);
		return -1;
	}

	if (getc(fp) != '\n') {
		fatal(-1,"bad ppm");
		fclose(fp);
		return -1;
	}

	if (ww != w || hh != h) {
		fatal(-1,"wrong size ppm");
		fclose(fp);
		return -1;
	}

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			u8 pix[3];
			u16 raw;
			u32 x0, x1, y0, y1, off;

			x0 = x & 3;
			x1 = x >> 2;
			y0 = y & 3;
			y1 = y >> 2;
			off = x0 + 4 * y0 + 16 * x1 + 4 * w * y1;

			if (fread(pix, 3, 1, fp) != 1) {
				fatal(-1, "read %s", name);
				fclose(fp);
				return -1;
			}

			raw = (pix[0] & 0xf8) << 7;
			raw |= (pix[1] & 0xf8) << 2;
			raw |= (pix[2] & 0xf8) >> 3;
			raw |= 0x8000;

			wbe16(data + 2*off, raw);
		}

	fclose(fp);
	return 0;
}

static u8 perm_from_path(const char *path)
{
	struct stat sb;
	mode_t mode;
	u8 perm;
	u32 i;

	if (stat(path, &sb))
		deadly(-1,"stat %s", path);

	perm = 0;
	mode = sb.st_mode;
	for (i = 0; i < 3; i++) {
		perm <<= 2;
		if (mode & 0200)
			perm |= 2;
		if (mode & 0400)
			perm |= 1;
		mode <<= 3;
	}

	return perm;
}

static int do_file_header(u64 title_id)
{
	u8 md5_calc[16];
	FILE *in;
	FILE * xp;
	char name[256];
	u32 i;

	memset(header, 0, sizeof header);

	wbe64(header, title_id);
	header[0x0c] = perm_from_path(".");
	memcpy(header + 0x0e, md5_blanker, 16);
	memcpy(header + 0x20, "WIBN", 4);
	// XXX: what about the stuff at 0x24?

	in = fopen("###title###", "rb");
	if (!in) {
		fatal(-1,"error opening ###title###"); 
		fclose(in);
		return -1;
	}
	if (fread(header + 0x40, 0x80, 1, in) != 1) {
		fatal(-2, "error reading ###title###");
		fclose(in);
		return -1;
	}
	fclose(in);

	if (read_image(header + 0xc0, 192, 64, "###banner###.ppm")) { return -1; }

	in = fopen("###icon###.ppm", "rb");
	if (in) {
		fclose(in);
		wbe32(header + 8, 0x72a0);
		if (read_image(header + 0x60c0, 48, 48, "###icon###.ppm")) { return -1; }
	} else {
		//wbe32(header + 8, 0xf0a0);

		for (i = 0; i < 8; i++) {
	 		snprintf(name, sizeof name, "###icon%d###.ppm", i);
			xp = fopen(name, "rb");
			if (xp) {
				fclose(xp);
				if (read_image(header + 0x60c0 + 0x1200*i, 48, 48, name)) { return -1;}
			} else break;
		}
		wbe32(header + 8, 0x60a0 + 0x1200*i);
	}

	md5(header, sizeof header, md5_calc);
	memcpy(header + 0x0e, md5_calc, 16);
	aes_cbc_enc(sd_key, sd_iv, header, sizeof header, header);

	if (fwrite(header, 0xf0c0, 1, fp) != 1) {
		fatal(-15,"write header");
		return -1;
	}

	return 0;
}

static int find_files_recursive(const char *path)
{
	DIR *dir;
	struct dirent *de;
	char name[53];
	u32 len;
	int is_dir;
	u8 *p;
	struct stat sb;
	u32 size;

	dir = opendir(path ? path : ".");
	if (!dir) {
		fatal(-30, "opendir %s", path ? path : ".");
		return -1;
	}

	while ((de = readdir(dir))) {
		if (strcmp(de->d_name, ".") == 0)
			continue;
		if (strcmp(de->d_name, "..") == 0)
			continue;
		if (strncmp(de->d_name, "###", 3) == 0)
			continue;

		if (path == 0)
			len = snprintf(name, sizeof name, "%s", de->d_name);
		else
			len = snprintf(name, sizeof name, "%s/%s", path,
			               de->d_name);

		if (len >= sizeof name) {
			fatal(-40,"path too long");
			return -1;
		}

//		if (de->d_type != DT_REG && de->d_type != DT_DIR)
//			ERROR("not a regular file or a directory");

		//is_dir = (de->d_type == DT_DIR);
		//is_dir = (de->d_name != NULL);
		is_dir = (((*de).data).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)>0;

		if (is_dir)
			size = 0;
		else {	
			if (stat(name, &sb)){
				fatal(-25,"stat %s", name);
				return -1;
			}
			size = sb.st_size;
		}

		p = files[n_files++];
		wbe32(p, 0x3adf17e);
		wbe32(p + 4, size);
		p[8] = perm_from_path(name);
		p[0x0a] = is_dir ? 2 : 1;
		strcpy(p + 0x0b, name);
		// maybe fill up with dirt

		size = round_up(size, 0x40);
		files_size += 0x80 + size;

//		if (de->d_type == DT_DIR)
		if (is_dir) {
			if(find_files_recursive(name)) {return -1;}
		}
	}

	if (closedir(dir)) {
		fatal(-20, "closedir");
		return -1;
	}
	return 0;
}

static int compar(const void *a, const void *b)
{
	return strcmp((char *)a + 0x0b, (char *)b + 0x0b);
}

static void find_files(void)
{
	n_files = 0;
	files_size = 0;

	memset(files, 0, sizeof files);

	find_files_recursive(0);

	qsort(files, n_files, 0x80, compar);
}

static int do_backup_header(u64 title_id)
{
	u8 header[0x80];

	memset(header, 0, sizeof header);

	wbe32(header, 0x70);
	wbe32(header + 4, 0x426b0001);
	wbe32(header + 8, ng_id);
	wbe32(header + 0x0c, n_files);
	wbe32(header + 0x10, files_size);
	wbe32(header + 0x1c, files_size + 0x3c0);

	wbe64(header + 0x60, title_id);
	memcpy(header + 0x68, ng_mac, 6);

	if (fwrite(header, sizeof header, 1, fp) != 1) {
		fatal(-50,"write Bk header");
		fclose(fp);
		return -1;
	}

	return 0;
}

static int do_file(u32 file_no)
{
	u8 *header;
	u32 size;
	u32 rounded_size;
	u8 perm, attr, type;
	char *name;
	u8 *data;
	FILE *in;
	char output[512];

	header = files[file_no];

	size = be32(header + 4);
	perm = header[8];
	attr = header[9];
	type = header[10];
	name = header + 11;

	if (verbose) {		
		sprintf(output,"file: size=%08x perm=%02x attr=%02x type=%02x name=%s\n",
		    size, perm, attr, type, name);
		set_savelib_output(output);
	}

	if (fwrite(header, 0x80, 1, fp) != 1) {
		fatal(-200,"write file header %d", file_no);
		return -1;
	}

	if (type == 1) {
		rounded_size = round_up(size, 0x40);

		data = malloc(rounded_size);
		if (!data) {
			fatal(-250,"malloc data");
			return -1;
		}

		in = fopen(name, "rb");
		if (!in) {
			fatal(-300, "open %s", name);
			return -1;
		}
		if (fread(data, size, 1, in) != 1) {
			fatal(-400, "read %s", name);
			return -1;
		}
		fclose(in);

		memset(data + size, 0, rounded_size - size);

		aes_cbc_enc(sd_key, header + 0x50, data, rounded_size, data);

		if (fwrite(data, rounded_size, 1, fp) != 1)
			fatal("write file %d", file_no);

		free(data);
	}
	return 0;
}

static void make_ec_cert(u8 *cert, u8 *sig, char *signer, char *name, u8 *priv,
                         u32 key_id)
{
	memset(cert, 0, 0x180);
	wbe32(cert, 0x10002);
	memcpy(cert + 4, sig, 60);
	strcpy(cert + 0x80, signer);
	wbe32(cert + 0xc0, 2);
	strcpy(cert + 0xc4, name);
	wbe32(cert + 0x104, key_id);
	ec_priv_to_pub(priv, cert + 0x108);
}

static int do_sig(char * full_path)
{
	u8 sig[0x40];
	u8 ng_cert[0x180];
	u8 ap_cert[0x180];
	u8 hash[0x14];
	u8 ap_priv[30];
	u8 ap_sig[60];
	char signer[64];
	char name[64];
	u8 *data;
	u32 data_size;
	size_t count;
	int fpos;

	sprintf(signer, "Root-CA00000001-MS00000002");
	sprintf(name, "NG%08x", ng_id);
	make_ec_cert(ng_cert, ng_sig, signer, name, ng_priv, ng_key_id);

	memset(ap_priv, 0, sizeof ap_priv);
	ap_priv[10] = 1;

	memset(ap_sig, 81, sizeof ap_sig);	// temp

	sprintf(signer, "Root-CA00000001-MS00000002-NG%08x", ng_id);
	sprintf(name, "AP%08x%08x", 1, 2);
	make_ec_cert(ap_cert, ap_sig, signer, name, ap_priv, 0);

	sha(ap_cert + 0x80, 0x100, hash);
	generate_ecdsa(ap_sig, ap_sig + 30, ng_priv, hash);
	make_ec_cert(ap_cert, ap_sig, signer, name, ap_priv, 0);

	data_size = files_size + 0x80;

	data = malloc(data_size);
	if (!data) {
		fatal(-500,"malloc");		
		return -1;
	}
	fpos = ftell(fp);
	fseek(fp, 0xf0c0, SEEK_SET);
	fpos = ftell(fp);
	if (fread(data, data_size, 1, fp) != 1) {
		fatal(-600, "read data for sig check");
		return -1;
	}
	fpos = ftell(fp);
	sha(data, data_size, hash);
	sha(hash, 20, hash);
	free(data);
	
	generate_ecdsa(sig, sig + 30, ap_priv, hash);
	wbe32(sig + 60, 0x2f536969);

	fclose(fp);

	fp = fopen(full_path, "ab+");
	if (!fp) {
		fatal(-700,"open %s", full_path);
		return -1;
	}

	//fseek(fp, 0x0, SEEK_END);
	count = fwrite(sig, sizeof sig, 1, fp);
	if (count != 1) {
		fatal(-800, "write sig");
		return -1;
	}
	count = fwrite(ng_cert, sizeof ng_cert, 1, fp);
	if ( count != 1) {
		fatal(-900, "write NG cert");
		return -1;
	}
	count = fwrite(ap_cert, sizeof ap_cert, 1, fp);
	if (count != 1) {
		fatal(-1000,"write AP cert");
		return -1;
	}

	return 0;
}

extern int twintig(int useBannerBin, char * app_folder, char * sourcedir, char * sourcefolder,  char * dataFileName)
{
	u64 title_id;
	u8 tmp[4];
	u32 i;
	char fullpath[512]; 
	int ret_code;

	//if (argc != 3) {
	//	fprintf(stderr, "Usage: %s <srcdir> <data.bin>\n", argv[0]);
	//	return 1;
	//}

	if(get_key(app_folder, "shared","sd-key", sd_key, 16)) { return -1;}
	if(get_key(app_folder, "shared","sd-iv", sd_iv, 16)) { return -1;}
	if(get_key(app_folder, "shared","md5-blanker", md5_blanker, 16)) { return -1;}

	if(get_key(app_folder, "private","NG-id", tmp, 4)) { return -1;}
	ng_id = be32(tmp);
	if(get_key(app_folder, "private","NG-key-id", tmp, 4)) { return -1;}
	ng_key_id = be32(tmp);
	if(get_key(app_folder, "private","NG-mac", ng_mac, 6)) { return -1;}
	if(get_key(app_folder, "private","NG-priv", ng_priv, 30)) { return -1;}
	if(get_key(app_folder, "private","NG-sig", ng_sig, 60)) { return -1;}

	if (sscanf(sourcefolder, "%016llx", &title_id) != 1) {
		fatal(-10,"Not a correct title id");
		return -1;
	}
	   
	strcpy(fullpath, sourcedir);
	strcat(fullpath, "\\..\\" );
	strcat(fullpath,dataFileName);
	//fp = fopen(fullpath, "wb+");
	fp = fopen(fullpath, "wb+");
	if (!fp) {
		fatal(-6, "open %s", dataFileName);
		return -1;
	}

	if (chdir(sourcedir)) {
		fatal(-5,"chdir %s", sourcedir);
		fclose(fp);
		return -1;
	}

	ret_code = do_file_header(title_id);

	if (ret_code) { 
		fclose(fp);
		return ret_code;
	}

	find_files();

	do_backup_header(title_id);

	for (i = 0; i < n_files; i++)
		do_file(i);

//	if (chdir(".."))
//		fatal("chdir ..");

	do_sig(fullpath);

	fclose(fp);

	return 0;
}
