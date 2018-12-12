#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; version 2 of the License.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#

rust_lib = librustlibs.la

noinst_LTLIBRARIES += $(rust_lib)

librustlibs_la_SOURCES = ""

CARGO_TARGET_DIR := $(abs_top_builddir)/lib/rust

$(rust_lib):
	CARGO_TARGET_DIR=$(CARGO_TARGET_DIR) cargo build --release --manifest-path $(top_srcdir)/lib/rust/Cargo.toml $(CARGO_OPTS)
	ln -sf $(CARGO_TARGET_DIR)/$(RUSTC_TARGET_NAME)/release/librustlibs.a
	ln -sf $(CARGO_TARGET_DIR)/$(RUSTC_TARGET_NAME)/release/librustlibs.la
	ln -sf $(CARGO_TARGET_DIR)/$(RUSTC_TARGET_NAME)/release/.libs

clean-local:
	CARGO_TARGET_DIR=$(CARGO_TARGET_DIR) cargo clean --manifest-path $(top_srcdir)/lib/rust/Cargo.toml
