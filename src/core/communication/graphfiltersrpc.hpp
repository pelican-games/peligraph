#pragma once

#include <string_view>

namespace Pelican {

class PathResolver;
class RpcServer;

inline constexpr std::string_view graphFiltersExternalModificationCode = "external_modification";
inline constexpr std::string_view graphFiltersInvalidCode = "project.graph_filters_invalid@1";
inline constexpr std::string_view graphFiltersTransactionBusyCode = "project.graph_filters_transaction_busy@1";

// get_graph_filters {} -> {digest, document, error}. A missing file is all null;
// invalid bytes have a digest and error but no document; unreadable bytes have
// null digest and document and an error.
//
// edit_graph_filters {base_digest: string|null, document: object} -> {committed,
// changed, digest, previous_digest, previous, document}. Null base_digest requires
// a missing file. The candidate and the current file are parsed strictly and the
// candidate is rendered canonically through commitProjectDocuments. Identical
// rendered bytes return changed:false without writing. Refusals write nothing:
// external_modification {expected, actual}, project.graph_filters_invalid@1
// {reason}, project.graph_filters_transaction_busy@1 {path}. Invalid existing
// bytes cannot be replaced by this RPC. There is no delete operation.
void configureGraphFiltersRpcHandlers(RpcServer &server, PathResolver &path_resolver);

} // namespace Pelican
