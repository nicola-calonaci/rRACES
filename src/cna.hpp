/*
 * This file is part of the rRACES (https://github.com/caravagnalab/rRACES/).
 * Copyright (c) 2023-2024 Alberto Casagrande <alberto.casagrande@uniud.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __RRACES_CNA__
#define __RRACES_CNA__

#include <string>

#include <cna.hpp>

#include <Rcpp.h>


class CNA : public Races::Mutations::CopyNumberAlteration
{
    // amplification
    CNA(const Races::Mutations::GenomicRegion& region,
        const Races::Mutations::AlleleId& allele,
        const Races::Mutations::AlleleId& src_allele);

    // deleletion
    CNA(const Races::Mutations::GenomicRegion& region,
        const Races::Mutations::AlleleId& allele);
public:
    CNA();

    inline std::string get_chromosome() const
    {
        return Races::Mutations::GenomicPosition::chrtos(region.get_chromosome_id());
    }

    inline const Races::Mutations::ChrPosition& get_position_in_chromosome() const
    {
        return region.get_initial_position();
    }

    inline size_t get_length() const
    {
        return region.size();
    }

    SEXP get_src_allele() const;

    SEXP get_allele() const;

    inline std::string get_type() const
    {
        if (type == Races::Mutations::CopyNumberAlteration::Type::AMPLIFICATION) {
            return "A";
        }
        return "D";
    }

    Rcpp::List get_dataframe() const;

    void show() const;

    static
    CNA build_CNA(const std::string type,
                  const SEXP chr, const SEXP pos_in_chr,
                  const SEXP length, const SEXP allele,
                  const SEXP src_allele);

    static
    inline CNA
    build_amplification(const SEXP chr, const SEXP pos_in_chr,
                        const SEXP length, const SEXP allele,
                        const SEXP src_allele)
    {
        return build_CNA("A", chr, pos_in_chr, length, allele, src_allele);
    }
    static
    inline CNA
    build_deletion(const SEXP chr, const SEXP pos_in_chr,
                   const SEXP length, const SEXP allele)
    {
        return build_CNA("D", chr, pos_in_chr, length, allele, allele);
    }
};


RCPP_EXPOSED_CLASS(CNA)

#endif // __RRACES_CNA__
