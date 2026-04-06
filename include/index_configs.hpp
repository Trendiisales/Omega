#pragma once
#include <string>

struct IndexConfig
{
    const char* symbol;
    double vol_target;
    double breakout_threshold;
};

static const IndexConfig IDX_CFG_SP =
{
    "US500",
    0.04,
    0.01
};

static const IndexConfig IDX_CFG_NQ =
{
    "NAS100",
    0.05,
    0.01
};

static const IndexConfig IDX_CFG_DJ =
{
    "DJ30",
    0.04,
    0.01
};

static const IndexConfig IDX_CFG_DAX =
{
    "GER40",
    0.05,
    0.01
};

static const IndexConfig IDX_CFG_UK =
{
    "UK100",
    0.04,
    0.01
};

static const IndexConfig IDX_CFG_ESTX =
{
    "ESTX50",
    0.04,
    0.01
};
