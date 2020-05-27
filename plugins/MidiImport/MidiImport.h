/*
 * MidiImport.h - support for importing MIDI-files
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef _MIDI_IMPORT_H
#define _MIDI_IMPORT_H

#include <QString>

#include "ImportFilter.h"

/*---------------------------------------------------------------------------*/

//! MIDI importing base class
class MidiImport : public ImportFilter
{
	Q_OBJECT

public:
	//! Build a MidiImport object from file designated by \param filename
	MidiImport(const QString &filename);

	//! Necessary for lmms_plugin_main()
	PluginView * instantiateView(QWidget *) override
	{
		return nullptr;
	}

private:
	//! Import MIDI data from MIDI file
	//! \param tc Container to receive MIDI tracks
	//! \return If operation was successful
	bool tryImport(TrackContainer *tc) override;

	//! Read file in Standard MIDI File (SMF) format
	bool readSMF(TrackContainer &tc);

	//! Read file in RIFF MIDI file format
	bool readRIFF(TrackContainer &tc);

	//! Read 32-bit word
	inline uint32_t read32LE()
	{
		uint32_t value = readByte();
		value |= readByte() << 8;
		value |= readByte() << 16;
		value |= readByte() << 24;
		return value;
	}

	//! Read four-byte identifier
	inline uint32_t readID()
	{
		return read32LE();
	}

	//! Read and discard <len> bytes
	inline void skip(int len)
	{
		for (int i = 0; i < len; ++i) { readByte(); }
	}
};

/*---------------------------------------------------------------------------*/

#endif
