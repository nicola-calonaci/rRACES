
# rRACES <a href="caravagnalab.github.io/rRACES"><img src="man/figures/logo.png" align="right" height="120" alt="" /></a>

<!-- badges: start -->
<!--
[![R-CMD-check](https://github.com/caravagnalab/rRACES/workflows/R-CMD-check/badge.svg)](https://github.com/caravagnalab/rRACES/actions)
[![pkgdown](https://github.com/caravagnalab/rRACES/actions/workflows/pkgdown.yaml/badge.svg)](https://github.com/caravagnalab/rRACES/actions/workflows/pkgdown.yaml)
-->
<!-- badges: end -->

`rRACES` is the R wrapper for
[RACES](https://github.com/albertocasagrande/RACES), a C++ tumour
evolution simulator.

#### Help and support

## [![](https://img.shields.io/badge/GitHub%20Pages-https://caravagnalab.github.io/rRACES/-yellow.svg)](https://caravagnalab.github.io/rRACES/)

### Installation

You can install the released version of `rRACES` by using `devtools` package with:

``` r
# install.packages("devtools")
devtools::install_github("caravagnalab/rRACES")
```

or clone the repository, build `rRACES` package, and install it:

``` shell
git clone https://github.com/caravagnalab/rRACES.git
R CMD build rRACES
R CMD install rRACES_*.tar.gz
```

Please, notice that plotting may require the R package 
`hexbin` under GNU/Linux.

------------------------------------------------------------------------

#### Copyright and contacts

Giulio Caravagna, Alberto Casagrande. Cancer Data Science (CDS)
Laboratory.

[![](https://img.shields.io/badge/CDS%20Lab%20Github-caravagnalab-seagreen.svg)](https://github.com/caravagnalab)
[![](https://img.shields.io/badge/CDS%20Lab%20webpage-https://www.caravagnalab.org/-red.svg)](https://www.caravagnalab.org/)
