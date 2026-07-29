# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
# This file is part of the Collabora Office project.
#
# SPDX-License-Identifier: MPL-2.0
#

$(eval $(call gb_Library_Library,pythoncompute))

$(eval $(call gb_Library_set_componentfile,pythoncompute,scaddins/source/pythoncompute/pythoncompute,services))

$(eval $(call gb_Library_set_include,pythoncompute,\
    $$(INCLUDE) \
    -I$(SRCDIR)/scaddins/inc \
    -I$(SRCDIR)/scaddins/source/pythoncompute \
))

$(eval $(call gb_Library_use_common_precompiled_header,pythoncompute))

$(eval $(call gb_Library_use_internal_comprehensive_api,pythoncompute,\
	offapi \
	scaddins \
	udkapi \
))

$(eval $(call gb_Library_use_libraries,pythoncompute,\
	comphelper \
	cppu \
	cppuhelper \
	sal \
	salhelper \
	tl \
	vcl \
))

$(eval $(call gb_Library_add_exception_objects,pythoncompute,\
	scaddins/source/pythoncompute/addin \
	scaddins/source/pythoncompute/anyjson \
	scaddins/source/pythoncompute/bridge \
	scaddins/source/pythoncompute/volatile \
))

# vim: set noet sw=4 ts=4:
