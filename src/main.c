#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <zlib.h>
#include "../incl/minimap2/minimap.h"
#include "../incl/minimap2/mmpriv.h"
#include "../incl/klib/kseq.h"
#include "../incl/klib/kvec.h"
#include "../incl/klib/khash.h"
#include "taxonomy.h"
#include <getopt.h>
#include <string.h>

KSEQ_INIT(gzFile, gzread)

typedef struct covarray {
  uint8_t* cov;
  uint32_t n;
} covarray;

KHASH_MAP_INIT_INT(ref2covg, covarray); // map a taxid (here a 32-bit int, in reality a size_t [uint32_t], but the hash is the same)

void usage() {
  printf("Usage: cthulhu [options]\n");
  printf("Options:\n");
  printf("  -q: FASTA/Q[.gz] file with reads\n");
  printf("  -r: Reference FASTA/Q[.gz] or precomputed index file\n");
  printf("  -d: Directory with *.dmp taxonomy files (NCBI taxonomy)\n");
  printf("  -t: Threads (default: 1)\n");
  printf("  -m: Maximum memory target (GB) (default: 32)\n");
  printf("  -s: Summary output file\n");
  printf("  -o: Read classification output\n");
  printf("  -i: Save index file\n");
  printf("  -p: Sequence type preset\n");
  printf("      map-ont: Oxford Nanopore (default)\n");
  printf("      map-pb:  Pacbio\n");
  printf("      sr:      Short reads (Illumina)\n");
  printf("  -f, --align-fraction: Portion of a read that must align properly (default: 0.5)\n");
  printf("  -a, --align-accuracy: Minimum accuracy of aligned portion of a read (default: 0.7)\n");
  printf("  -c, --careful: Compute more exact alignments and coverage profiles, increases time and memory\n");
  printf("  -v, --verbose: verbose\n");
  printf("  -h, --help: show this\n");
}

static struct option long_options[] = {
// if these are the same as a single-character option, put that character in the 4th field instead of 0
  { "align-fraction",         required_argument, 0, 'f' },
  { "align-accuracy",         required_argument, 0, 'a' },
  { "careful",                no_argument,       0, 'c' },
  { "verbose",                no_argument,       0, 'v' },
  { "help",                   no_argument,       0, 'h' },
  { 0, 0, 0, 0}
};

int main(int argc, char *argv[]) {
  mm_idxopt_t iopt;
  mm_mapopt_t mopt;

  char* read_fasta = NULL;
  char* ref_fasta = NULL;
  char* tax_dir = NULL;
  char* preset = "map-ont";
  char* out_file = NULL;
  char* summary_file = NULL;
  char* idx_file = NULL;
  float align_fraction = 0.5;
  float align_accuracy = 0.7;
  int n_threads = 1;
  int verbose = 0;
  int careful = 0;
  int target_memory_gb = 32;
  size_t covg_bin_size = 1000; // bin size in coverage arrays

  int opt, long_idx;
  opterr = 0;
  while ((opt = getopt_long(argc, argv, "q:r:d:t:f:a:p:s:o:m:i:vh", long_options, &long_idx)) != -1) {
    switch (opt) {
      case 'q':
        read_fasta = optarg;
        break;
      case 'r':
        ref_fasta = optarg;
        break;
      case 'd':
        tax_dir = optarg;
        break;
      case 't':
        n_threads = atoi(optarg);
        break;
      case 'p':
        preset = optarg;
        break;
      case 'm':
        target_memory_gb = atoi(optarg);
        break;
      case 'f':
        align_fraction = atof(optarg);
        break;
      case 'a':
        align_accuracy = atof(optarg);
        break;
      case 's':
        summary_file = optarg;
        break;
      case 'o':
        out_file = optarg;
        break;
      case 'i':
        idx_file = optarg;
        break;
      case 'v':
        verbose = 1;
        break;
      case 'c':
        careful = 1;
        break;
      case 'h':
        usage();
        return 0;
        break;
      case '?':
        if (optopt == 'q' || optopt == 'r' || optopt == 'd' || optopt == 't' || optopt == 'f' || optopt == 'a' || optopt == 'p' || optopt == 'm' || optopt == 'o' || optopt == 's' || optopt == 'i')
          fprintf(stderr, "Option -%c requires an argument.\n", optopt);
        else if (isprint (optopt))
          fprintf(stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
        return 1;
      case 0:
        // as long as all the long arguments have characters too, I don't think this section will be used
        if (long_idx == 0) align_fraction = atof(optarg); // --align-fraction
        else if (long_idx == 1) align_accuracy = atof(optarg); // --align-accuracy
        else if (long_idx == 2) careful = 1; // --careful
        else if (long_idx == 3) verbose = 1; // --verbose
        else if (long_idx == 4) {usage(); return 0;} // --help
      default:
        usage();
        return 1;
    }
  }

  if(tax_dir == NULL) {
    fprintf(stderr, "-d taxonomy directory is required\n");
    return 1;
  }
  if(read_fasta == NULL) {
    fprintf(stderr, "-q reads FASTA is required\n");
    return 1;
  }
  if(ref_fasta == NULL) {
    fprintf(stderr, "-r reference FASTA is required\n");
    return 1;
  }
  if(out_file == NULL && summary_file == NULL) {
    fprintf(stderr, "Specify -o, -s, or both, otherwise we're doing all this for nothing\n");
    return 1;
  }

  mm_verbose = 2; // disable message output to stderr
  mm_set_opt(0, &iopt, &mopt); // initialize with defaults
  mm_set_opt(preset, &iopt, &mopt); // then add ont presets
  // this isn't great, but it's close-ish
  if(target_memory_gb < 10) {
    iopt.batch_size = target_memory_gb * 100000000ULL;
  } else if(target_memory_gb < 50) {
    iopt.batch_size = target_memory_gb * 200000000ULL;
  } else if(target_memory_gb < 100) {
    iopt.batch_size = target_memory_gb * 250000000ULL;
  } else if(target_memory_gb < 200) {
    iopt.batch_size = target_memory_gb * 300000000ULL;
  } else {
    iopt.batch_size = target_memory_gb * 400000000ULL;
  }
  fprintf(stderr, "Target memory usage: %d GB\n", target_memory_gb);
  fprintf(stderr, "Using batch size: %d Gbp [expect ~(40 / batch size) Gbp for Refseq]\n", iopt.batch_size/1000000000);
  if(careful)
    mopt.flag |= MM_F_CIGAR; // perform alignment

  char* name_f = malloc((strlen(tax_dir)+10) * sizeof(char));
  strcpy(name_f, tax_dir);
  strcat(name_f, "/names.dmp");

  char* node_f = malloc((strlen(tax_dir)+10) * sizeof(char));
  strcpy(node_f, tax_dir);
  strcat(node_f, "/nodes.dmp");

  taxonomy* tax = read_taxonomy(name_f, node_f);

  char* acc2tax_f = malloc((strlen(tax_dir)+24) * sizeof(char));
  strcpy(acc2tax_f, tax_dir);
  strcat(acc2tax_f, "/nucl_gb.accession2taxid.filtered");

  khash_t(acc2tax) *a2tx = parse_acc2tax(acc2tax_f);
  fprintf(stderr, "Parsed taxonomy files.\n");

  khash_t(ref2covg) *r2cv = kh_init(ref2covg); // tax id to (binned) coverage array

  // open query file for reading; you may use your favorite FASTA/Q parser
  gzFile f;
  kseq_t *ks;

  // open index reader
  fprintf(stderr, "Building mm2 index...\n");
  mm_idx_reader_t *r = mm_idx_reader_open(ref_fasta, &iopt, idx_file);
  mm_idx_t *mi;
  khint_t bin; // hash bin (result of kh_put/get)
  int absent;
  size_t taxid;
  kvec_t(int) read_taxa;
  kv_init(read_taxa);
  while ((mi = mm_idx_reader_read(r, n_threads)) != 0) { // traverse each part of the index
    // open (or re-open) the query file -- needs to be re-read through for each part of the index
    f = gzopen(read_fasta, "r");
    assert(f);
    ks = kseq_init(f); 

    fprintf(stderr, "Processing mm2 index (or fraction thereof)...\n");
    mm_mapopt_update(&mopt, mi); // this sets the maximum minimizer occurrence; TODO: set a better default in mm_mapopt_init()!
    mm_tbuf_t *tbuf = mm_tbuf_init(); // thread buffer; for multi-threading, allocate one tbuf for each thread
    int n = 0;
    while (kseq_read(ks) >= 0) { // each kseq_read() call reads one query sequence
      if(n == kv_size(read_taxa)) {
        kv_push(int, read_taxa, 0);
      }
      mm_reg1_t *reg;
      int j, i, n_reg;
      if(verbose) {
        fprintf(stderr, "Processing read %d (%s)\n", n, ks->name.s);
      }
      reg = mm_map(mi, ks->seq.l, ks->seq.s, &n_reg, tbuf, &mopt, 0); // get all hits for the query
      if(verbose) {
        fprintf(stderr, "  %d raw alignments\n", n_reg);
      }
      for (j = 0; j < n_reg; ++j) { // traverse hits and print them out
        mm_reg1_t *r = &reg[j];
        if(careful)
          assert(r->p); // with MM_F_CIGAR, this should not be NULL

        int aln_len = r->qe - r->qs;
        float aln_frac = (float)aln_len / ks->seq.l;
        float accuracy = (float)r->mlen / r->blen;
        if(verbose) {
          fprintf(stderr, "    matches ref %d (%d-%d)\n", r->rid, r->rs, r->re);
        }

        // arbitrary thresholds right now - these should be parameterized
        if(aln_frac > align_fraction && accuracy > align_accuracy) {

          bin = kh_get(acc2tax, a2tx, mi->seq[r->rid].name);
          absent = (bin == kh_end(a2tx)); 
          if(absent) {
            fprintf(stderr, "Target/accession ID '%s' not found in acc2tax\n", mi->seq[r->rid].name);
          } else {
            taxid = kh_val(a2tx, bin);
            if(verbose) {
              fprintf(stderr, "      matches taxon %d (%s)\n", taxid, tax->names[taxid]);
            }
          }

          if(kv_A(read_taxa, n) == 0)
            kv_A(read_taxa, n) = taxid;
          else
            kv_A(read_taxa, n) = lca(taxid, kv_A(read_taxa, n), tax);
        
          // only do pileup on the first primary alignment
          if(j == 0) {
            bin = kh_put(ref2covg, r2cv, r->rid, &absent);
            if(absent) {
              kh_val(r2cv, bin).n = mi->seq[r->rid].len / covg_bin_size + 1;
              if(verbose) {
                fprintf(stderr, "Making new coverage array of length %u (%u / %u) for ref %d\n", kh_val(r2cv, bin).n, mi->seq[r->rid].len, covg_bin_size, r->rid);
              }
              kh_val(r2cv, bin).cov = calloc(sizeof(uint8_t), kh_val(r2cv, bin).n);
            }
            for(i = r->rs/covg_bin_size; i <= r->re/covg_bin_size; i++) {
              if(verbose) {
                fprintf(stderr, "Incrementing coverage in bin %d\n", i);
              }
              if(kh_val(r2cv, bin).cov[i] < 255) {
                kh_val(r2cv, bin).cov[i] += 1;
              }
            }
          }

          //printf("length %d, accuracy %f\n", aln_len, accuracy);
          if(0 && taxid != 9606) {
            printf("%s\t%d\t%d\t%d\t%c\t", ks->name.s, ks->seq.l, r->qs, r->qe, "+-"[r->rev]);
            printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\tcg:Z:", mi->seq[r->rid].name, mi->seq[r->rid].len, r->rs, r->re, r->mlen, r->blen, r->mapq);
            if(careful) {
              for (i = 0; i < r->p->n_cigar; ++i) { // IMPORTANT: this gives the CIGAR in the aligned regions. NO soft/hard clippings!
                printf("%d%c", r->p->cigar[i]>>4, "MIDSHN"[r->p->cigar[i]&0xf]);
              }
            }
            printf("\n");
          }
        }
        free(r->p);
      }
      n++;
      free(reg);
    }
    fprintf(stderr, "%d reads processed\n", n);
    mm_tbuf_destroy(tbuf);
    mm_idx_destroy(mi);
    kseq_destroy(ks); // close the query file
    gzclose(f);
  }
  mm_idx_reader_close(r); // close the index reader

  // clean up a2tx (acc2tax)
  for (bin = 0; bin < kh_end(a2tx); ++bin) {
    if (kh_exist(a2tx, bin))
      free((char*)kh_key(a2tx, bin));
  }
  kh_destroy(acc2tax, a2tx);

  // count reads per taxa
  taxtree *tree = new_tree();
  int i, j;
  int no_hit = 0;
  FILE* o = out_file != NULL ? fopen(out_file, "w") : (FILE*)NULL;
  for(i = 0; i < kv_size(read_taxa); i++) {
    if(kv_A(read_taxa, i) == 0) {
      no_hit++;
    } else {
      // output taxa result for this read
      if(out_file != NULL) {
        fprintf(o, "%d\t%d\t%s\n", i, kv_A(read_taxa, i), tax->names[kv_A(read_taxa, i)]);
      }

      if(summary_file != NULL) {
        add_to_tree(tax, tree, kv_A(read_taxa, i));
      }
    }
  }
  if(summary_file != NULL) {
    FILE* sf = fopen(summary_file, "w");
    fprintf(sf, "Taxa:\n");
    fprintf(sf, "0\t%d\t%d\tno hit\n", no_hit, no_hit);

    // output single taxa counts and build full hierarchal tree
    for (bin = 0; bin < kh_end(tree); ++bin) {
      if (kh_exist(tree, bin)) {
        taxid = kh_key(tree, bin);
        fprintf(sf, "%d\t%d\t%d\t%s\n", taxid, kh_val(tree, bin).count, kh_val(tree, bin).unique_count, tax->names[taxid]);
      }
    }
    fprintf(sf, "\n");

    // print coverage info
    fprintf(sf, "Coverage:\n");
    for (bin = 0; bin < kh_end(r2cv); ++bin) {
      if (kh_exist(r2cv, bin)) {
        uint8_t* c = kh_val(r2cv, bin).cov;
        uint32_t n = kh_val(r2cv, bin).n;
        uint32_t total = 0;
        uint32_t covered = 0;
        uint32_t refid = kh_key(r2cv, bin);
        for(j = 0; j < n; j++) {
          if(c[j] > 0) {
            total += c[j];
            covered += 1;
          }
        }
        fprintf(sf, "%u\t%u\t%u\t%u\t%f\t%f\t%f\n", refid, n * covg_bin_size, total * covg_bin_size, covered * covg_bin_size, (float)covered/n, (float)total/n, (float)total/covered);
      }
    }
    fprintf(sf, "\n");

    fprintf(sf, "Tree:\n");
    depth_first_traverse(tax, tree, 1, 0, sf); // do a depth-first tree render starting at the root
  }


  // clean up memory
  kh_destroy(nodehash, tree);
  kv_destroy(read_taxa);
  free(name_f);
  free(node_f);
  free(acc2tax_f);
  free_tax(tax);

  return 0;
}

/*

^(;,;)^

 */
