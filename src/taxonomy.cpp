#include "dada.h"
#include <Rcpp.h>
using namespace Rcpp;

// Gets kmer index
// Returns -1 if non-ACGT base encountered
int tax_kmer(const char *seq, unsigned int k) {
  unsigned int j, nti;
  int kmer=0;
  
  for(j=0; j<k; j++) {
    if(seq[j] == 'A') {
      nti = 0;
    } else if (seq[j] == 'C') {
      nti = 1;
    } else if (seq[j] == 'G') {
      nti = 2;
    } else if (seq[j] == 'T') {
      nti = 3;
    } else {
      kmer = -1;
      break;
    }
    kmer = 4*kmer + nti;
  }
  return(kmer);
}

void tax_kvec(const char *seq, unsigned int k, unsigned char *kvec) {
  unsigned int i;
  unsigned int len = strlen(seq);
  int kmer = 0;
  size_t n_kmers = (1 << (2*k));  // 4^k kmers
  for(i=0;i<n_kmers;i++) { kvec[i] = 0; }

  for(i=0; i<len-k; i++) {
    kmer = tax_kmer(&seq[i], k);
    
    // Ensure a valid kmer index
    if(kmer>=0 && kmer<n_kmers) {
      kvec[kmer] = 1;
    }
  }
}

unsigned int tax_karray(const char *seq, unsigned int k, int *karray) {
  unsigned int i, j;
  int kmer;
  unsigned int len = strlen(seq);
  
  for(i=0,j=0;i<len-k;i++) {
    kmer = tax_kmer(&seq[i], k);
    // Ensure a valid kmer index
    if(kmer>=0) {
      karray[j] = kmer;
      j++;
    }
  }
  return(j);
}

int get_best_genus(int *karray, unsigned int arraylen, unsigned int n_kmers, unsigned int *genus_kmers, unsigned int ngenus, double *kmer_prior, double *genus_num_plus1) {
  unsigned int pos;
  unsigned int *genus_kv;
  int kmer, g, max_g = -1;
  unsigned int log_step = 50; ///! Need log10(ngenus+1) * log_step < 300 (max double ~ 10^308)
  double p, logp, max_logp = 1.0; // Init value to be replaced on first iteration
    
  for(g=0;g<ngenus;g++) {
    genus_kv = &genus_kmers[g*n_kmers];
    logp = 0.0;
    p = 1.0;
    
    // Take the product of the numerators
    // Convert to log to avoid double overflow
    for(pos=0;pos<arraylen;pos++) {
      kmer = karray[pos];
      if(kmer < 0) { Rcpp::stop("Sequences to be classifed must be ACGT only."); }
      p *= (genus_kv[kmer] + kmer_prior[kmer]);
      if((pos+1) % log_step == 0) {
        logp += log(p);
        p = 1.0;
      }
    }
    logp += log(p);
    // Subtract the product of the denominators
    logp = logp - (arraylen * log(genus_num_plus1[g]));
    
    // Store if new max
    if(max_logp > 0 || logp>max_logp) {
      max_logp = logp;
      max_g = g;
    }
  }
  return max_g;
}

//------------------------------------------------------------------
// Assigns taxonomy to sequence based on provided ref seqs and corresponding taxonomies.
//
// [[Rcpp::export]]
Rcpp::List C_assign_taxonomy(std::vector<std::string> seqs, std::vector<std::string> refs, std::vector<int> ref_to_genus, Rcpp::IntegerMatrix genusmat, bool verbose) {
  size_t i, j, g;
  int kmer;
  unsigned int k=8;
  size_t n_kmers = (1 << (2*k));
  size_t nseq = seqs.size();
  if(nseq == 0) Rcpp::stop("No seqs provided to classify.");
  size_t nref = refs.size();
  if(nref != ref_to_genus.size()) Rcpp::stop("Length mismatch between number of references and map to genus.");
  size_t ngenus = genusmat.nrow();

  // Rprintf("Validated and 0-index ref_to_genus map.\n");
  for(i=0;i<ref_to_genus.size();i++) {
    ref_to_genus[i] = ref_to_genus[i]-1; // -> 0-index
    if(ref_to_genus[i]<0 || ref_to_genus[i] >= ngenus) {
      Rcpp::stop("Invalid map from references to genus.");
    }
  }
  
  // Rprintf("Count seqs in each genus (M_g).\n");
  double *genus_num_plus1 = (double *) calloc(ngenus, sizeof(double));
  if(genus_num_plus1 == NULL) Rcpp::stop("Memory allocation failed.");  
  for(i=0;i<nref;i++) {
    genus_num_plus1[ref_to_genus[i]]++;
  }
  for(g=0;g<ngenus;g++) {
    genus_num_plus1[g]++;
  }
  
  unsigned int *genus_kmers = (unsigned int *) calloc((ngenus * n_kmers), sizeof(unsigned int));
  if(genus_kmers == NULL) Rcpp::stop("Memory allocation failed.");
  unsigned int *genus_kv;
  double *kmer_prior = (double *) calloc(n_kmers, sizeof(double));
  if(kmer_prior == NULL) Rcpp::stop("Memory allocation failed.");
  
  unsigned char *ref_kv = (unsigned char *) malloc(n_kmers * sizeof(unsigned char));
  if(ref_kv == NULL) Rcpp::stop("Memory allocation failed.");
  for(i=0;i<nref;i++) {
    // Calculate kmer-vector of this reference sequences
    tax_kvec(refs[i].c_str(), k, ref_kv);
    // Assign the kmer-counts to the appropriate "genus" and kmer-prior
    g = ref_to_genus[i];
    genus_kv = &genus_kmers[g*n_kmers];
    for(kmer=0;kmer<n_kmers;kmer++) {
      if(ref_kv[kmer]) { 
        genus_kv[kmer]++;
        kmer_prior[kmer]++;
      }
    }
  }
  
  // Correct word priors
  for(kmer=0;kmer<n_kmers;kmer++) {
    kmer_prior[kmer] = (kmer_prior[kmer] + 0.5)/(1.0 + nref);
  }
  if(verbose) { Rprintf("Finished processing reference fasta."); }
  
  // Rprintf("Get size of the kmer arrays for the sequences to be classified.\n");
  unsigned int max_arraylen = 0;
  unsigned int seqlen;
  for(i=0;i<nseq;i++) {
    seqlen = seqs[i].size();
    if(seqlen < 50) Rcpp::stop("Sequences must be at least 50 nts to classify.");
    if((seqlen-k) > max_arraylen) { max_arraylen = seqlen-k; }
  }

  // Rprintf("Allocate kmer array to be used by the seqs.");
  int *karray = (int *) malloc(max_arraylen * sizeof(int));
  if(karray == NULL) Rcpp::stop("Memory allocation failed.");

  Rcpp::IntegerVector rval(nseq);
  Rcpp::NumericVector unifs;
  Rcpp::IntegerMatrix rboot(nseq, genusmat.ncol());
  Rcpp::IntegerMatrix rboot_tax(nseq, 100);
  
  int max_g, boot_g;
  unsigned int boot, booti, boot_match, arraylen;
  
  // Rprintf("Allocate bootstrap array to be used by the seqs.\n");
  int *bootarray = (int *) malloc((max_arraylen/8) * sizeof(int));
  if(bootarray == NULL) Rcpp::stop("Memory allocation failed.");
  
  // Rprintf("Classify the sequences.\n");
  for(j=0;j<nseq;j++) {
    seqlen = seqs[j].size();
    arraylen = tax_karray(seqs[j].c_str(), k, karray);
    if(arraylen<40) { Rcpp::stop("Sequences must have at least 40 valid kmers to classify."); }
    
    // Find best hit
    max_g = get_best_genus(karray, arraylen, n_kmers, genus_kmers, ngenus, kmer_prior, genus_num_plus1);
    rval(j) = max_g+1; // 1-index for return
    
    // Generate random indices to be used for subsampling
    unifs = Rcpp::runif(100*(arraylen/8));
    booti = 0;
    boot_match = 0;
    for(boot=0;boot<100;boot++) {
      for(i=0;i<(arraylen/8);i++,booti++) {
        bootarray[i] = karray[(int) (arraylen*unifs[booti])];
      }
      boot_g = get_best_genus(bootarray, (arraylen/8), n_kmers, genus_kmers, ngenus, kmer_prior, genus_num_plus1);
      rboot_tax(j,boot) = boot_g+1; // 1-index for return
      for(i=0;i<(genusmat.ncol());i++) {
        if(genusmat(boot_g,i) == genusmat(max_g,i)) {
          rboot(j,i)++;
        } else {
          break;
        }
      }
      if(boot_g == max_g) { boot_match++; }
    }
    Rcpp::checkUserInterrupt();
  }
  
  free(genus_num_plus1);
  free(genus_kmers);
  free(kmer_prior);
  free(karray);
  
  return(Rcpp::List::create(_["tax"]=rval, _["boot"]=rboot, _["boot_tax"]=rboot_tax));
}
