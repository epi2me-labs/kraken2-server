#!/usr/bin/env python3
"""Build small synthetic kraken2 databases and matching reads for testing.

No downloads are needed: two related "species" are generated from a common
ancestor (so reads give species-level calls and, where the species agree,
genus-level LCA calls), along with reads from neither (unclassified). The
output is fully determined by the seed.

    make_test_databases.py BUILD_DB OUT_DIR

BUILD_DB is the `build_db` executable from the kraken2 build. In OUT_DIR it
creates:

    dna32/      DNA database, 32-bit hash cells (the usual kind)
    dna40/      DNA database, 40-bit hash cells
    protein/    protein database, for translated search
    dna.fastq   reads for the DNA databases
    protein.fastq   DNA reads for the protein database (searched in 6 frames)
"""
import os
import random
import subprocess
import sys

SEED = 20241124

# A tiny NCBI-style taxonomy:  root > Bacteria > Bacillaceae > Bacillus > {A, B}
NODES = [  # taxid, parent, rank
    (1, 1, "no rank"),
    (131567, 1, "no rank"),
    (2, 131567, "superkingdom"),
    (1239, 2, "phylum"),
    (91061, 1239, "class"),
    (1385, 91061, "order"),
    (186817, 1385, "family"),
    (1386, 186817, "genus"),
    (1423, 1386, "species"),
    (1396, 1386, "species"),
]
NAMES = {
    1: "root", 131567: "cellular organisms", 2: "Bacteria", 1239: "Firmicutes",
    91061: "Bacilli", 1385: "Bacillales", 186817: "Bacillaceae",
    1386: "Bacillus", 1423: "Bacillus testus", 1396: "Bacillus fictus",
}
SPECIES_A, SPECIES_B = 1423, 1396

CODONS = {  # amino acid -> codons (standard table, enough variety for back-translation)
    "A": ["GCT", "GCC", "GCA", "GCG"], "C": ["TGT", "TGC"], "D": ["GAT", "GAC"],
    "E": ["GAA", "GAG"], "F": ["TTT", "TTC"], "G": ["GGT", "GGC", "GGA", "GGG"],
    "H": ["CAT", "CAC"], "I": ["ATT", "ATC", "ATA"], "K": ["AAA", "AAG"],
    "L": ["TTA", "TTG", "CTT", "CTC", "CTA", "CTG"], "M": ["ATG"], "N": ["AAT", "AAC"],
    "P": ["CCT", "CCC", "CCA", "CCG"], "Q": ["CAA", "CAG"],
    "R": ["CGT", "CGC", "CGA", "CGG", "AGA", "AGG"],
    "S": ["TCT", "TCC", "TCA", "TCG", "AGT", "AGC"], "T": ["ACT", "ACC", "ACA", "ACG"],
    "V": ["GTT", "GTC", "GTA", "GTG"], "W": ["TGG"], "Y": ["TAT", "TAC"],
}
AMINO = sorted(CODONS)
COMP = str.maketrans("ACGT", "TGCA")


def revcomp(s):
    return s.translate(COMP)[::-1]


def mutate(seq, rate, alphabet, rng):
    out = list(seq)
    for i in range(len(out)):
        if rng.random() < rate:
            out[i] = rng.choice([c for c in alphabet if c != out[i]])
    return "".join(out)


def write_taxonomy(path):
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, "nodes.dmp"), "w") as f:
        for taxid, parent, rank in NODES:
            f.write(f"{taxid}\t|\t{parent}\t|\t{rank}\t|\t\t|\t0\t|\t0\t|\t0\t|\t0\t|\t0\t|\t0\t|\t0\t|\t0\t|\t\t|\n")
    with open(os.path.join(path, "names.dmp"), "w") as f:
        for taxid, name in NAMES.items():
            f.write(f"{taxid}\t|\t{name}\t|\t\t|\tscientific name\t|\n")


def build(build_db, outdir, taxdir, library, mapping, extra):
    os.makedirs(outdir, exist_ok=True)
    cmd = [build_db, "-H", os.path.join(outdir, "hash.k2d"),
           "-t", os.path.join(outdir, "taxo.k2d"),
           "-o", os.path.join(outdir, "opts.k2d"),
           "-n", taxdir, "-m", mapping] + extra
    with open(library) as lib:
        subprocess.run(cmd, stdin=lib, check=True, stdout=subprocess.DEVNULL)


def fastq(path, reads):
    with open(path, "w") as f:
        for name, seq, quals in reads:
            f.write(f"@{name}\n{seq}\n+\n{quals}\n")


def quals_for(n, rng):
    # a mix of good and poor base qualities
    return "".join(rng.choice("5?I+I?5I") for _ in range(n))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 64
    build_db, out = sys.argv[1], sys.argv[2]
    rng = random.Random(SEED)
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, "_work")
    os.makedirs(work, exist_ok=True)
    taxdir = os.path.join(work, "taxonomy")
    write_taxonomy(taxdir)

    # ---------------- DNA ----------------
    ancestor = "".join(rng.choice("ACGT") for _ in range(150_000))
    genomes = {SPECIES_A: mutate(ancestor, 0.02, "ACGT", rng),
               SPECIES_B: mutate(ancestor, 0.08, "ACGT", rng)}
    with open(os.path.join(work, "dna.fa"), "w") as lib, \
         open(os.path.join(work, "dna.map"), "w") as mp:
        for taxid, g in genomes.items():
            lib.write(f">seq{taxid}\n{g}\n")
            mp.write(f"seq{taxid}\t{taxid}\n")

    reads = []
    for i in range(3000):
        taxid = rng.choice([SPECIES_A, SPECIES_B])
        g = genomes[taxid]
        n = rng.randint(300, 4000)
        start = rng.randrange(len(g) - n)
        seq = mutate(g[start:start + n], 0.04, "ACGT", rng)
        if rng.random() < 0.5:
            seq = revcomp(seq)
        reads.append((f"r{i}_t{taxid}", seq, quals_for(n, rng)))
    for i in range(600):  # unrelated
        n = rng.randint(300, 3000)
        reads.append((f"u{i}", "".join(rng.choice("ACGT") for _ in range(n)),
                      quals_for(n, rng)))
    for i in range(200):  # chimeras of both species
        a, b = genomes[SPECIES_A], genomes[SPECIES_B]
        sa, sb = rng.randrange(len(a) - 800), rng.randrange(len(b) - 800)
        seq = a[sa:sa + 800] + b[sb:sb + 800]
        reads.append((f"c{i}", seq, quals_for(len(seq), rng)))
    for i in range(50):  # short, and with ambiguous bases
        seq = mutate(genomes[SPECIES_A][1000:1400], 0.0, "ACGT", rng)
        seq = "".join("N" if rng.random() < 0.05 else c for c in seq)
        reads.append((f"n{i}", seq, quals_for(len(seq), rng)))
    rng.shuffle(reads)
    fastq(os.path.join(out, "dna.fastq"), reads)

    dna_args = ["-k", "35", "-l", "31", "-c", "800000"]
    build(build_db, os.path.join(out, "dna32"), taxdir,
          os.path.join(work, "dna.fa"), os.path.join(work, "dna.map"), dna_args)
    build(build_db, os.path.join(out, "dna40"), taxdir,
          os.path.join(work, "dna.fa"), os.path.join(work, "dna.map"), dna_args + ["-C", "40"])

    # ---------------- protein ----------------
    ancestral = ["".join(rng.choice(AMINO) for _ in range(rng.randint(150, 400)))
                 for _ in range(250)]
    proteins = {SPECIES_A: [mutate(p, 0.03, AMINO, rng) for p in ancestral],
                SPECIES_B: [mutate(p, 0.15, AMINO, rng) for p in ancestral]}
    with open(os.path.join(work, "prot.fa"), "w") as lib, \
         open(os.path.join(work, "prot.map"), "w") as mp:
        for taxid, plist in proteins.items():
            for j, p in enumerate(plist):
                lib.write(f">p{taxid}_{j}\n{p}\n")
                mp.write(f"p{taxid}_{j}\t{taxid}\n")

    def backtranslate(p):
        return "".join(rng.choice(CODONS[a]) for a in p)

    reads = []
    for i in range(1500):
        taxid = rng.choice([SPECIES_A, SPECIES_B])
        p = rng.choice(proteins[taxid])
        flank = "".join(rng.choice("ACGT") for _ in range(rng.randint(0, 60)))
        seq = mutate(flank + backtranslate(p) + flank[::-1], 0.01, "ACGT", rng)
        if rng.random() < 0.5:
            seq = revcomp(seq)
        reads.append((f"pr{i}_t{taxid}", seq, quals_for(len(seq), rng)))
    for i in range(300):
        n = rng.randint(300, 1500)
        reads.append((f"pu{i}", "".join(rng.choice("ACGT") for _ in range(n)),
                      quals_for(n, rng)))
    rng.shuffle(reads)
    fastq(os.path.join(out, "protein.fastq"), reads)

    build(build_db, os.path.join(out, "protein"), taxdir,
          os.path.join(work, "prot.fa"), os.path.join(work, "prot.map"),
          ["-k", "15", "-l", "12", "-c", "400000", "-X"])

    for name in ("dna32", "dna40", "protein"):
        size = os.path.getsize(os.path.join(out, name, "hash.k2d"))
        print(f"{name}: hash.k2d {size/1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
