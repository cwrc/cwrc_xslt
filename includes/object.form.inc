<?php

/**
 * @file
 * Contains the implementation for adding a datastream using an xslt.
 */

/**
 * Islandora add datastream with XSLT form callback.
 */
function cwrc_xslt_object_datastream_form(array $form, array &$form_state, \IslandoraFedoraObject $object) {
  $values = !empty($form_state['values']) ? $form_state['values'] : array();
  $form['#islandora_object'] = $object;

  $form['introduction'] = array(
    '#markup' => t('<h2>Transform</h2>'),
  );

  // We are saving the options in form state to avoid subsequent retrieval of
  // datastreams of ajax callbacks.
  if (empty($form_state['source_datastream_options'])) {
    $id = $object->id;
    $object_datastreams = $object->repository->api->a->listDatastreams($id);
    $source_datastream_options = [];
    foreach ($object_datastreams as $dsid => $datastream) {
      if ($dsid === 'RELS-EXT' || !_cwrc_xslt_datastream_is_supported($datastream)) {
        continue;
      }
      $source_datastream_options[$dsid] = $datastream['label'];
    }
    $form_state['source_datastream_options'] = $source_datastream_options;
  }
  else {
    $source_datastream_options = $form_state['source_datastream_options'];
  }

  $form['source_datastream'] = array(
    '#type' => 'select',
    '#title' => t('Source Datastream'),
    '#required' => TRUE,
    '#options' => $source_datastream_options,
    '#ajax' => array(
      'callback' => 'cwrc_xslt_islandora_add_datastream_xslt_select_ajax_callback',
      'wrapper' => 'cwrc-xslt--select-xslt-options-wrapper',
      'effect' => 'fade',
    ),
  );

  $form['select_xslt'] = array(
    '#type' => 'item',
    '#title' => t('Select xslt'),
    '#prefix' => '<div id="cwrc-xslt--select-xslt-options-wrapper">',
    '#markup' => t('<p>Available XSLT will display here.</p>'),
    '#suffix' => '</div>',
  );
  if (!empty($values['source_datastream'])) {
    $source_datastream = $values['source_datastream'];
    $form['select_xslt']['#type'] = 'select';
    $form['select_xslt']['#required'] = TRUE;
    $form['select_xslt']['#options'] = _cwrc_xslt_get_options($source_datastream, $form_state);
  }

  if (empty($form_state['object_lock_acquired'])) {
    // Adding locks information - only adding this on foam load when no ajax
    // callback.
    form_load_include($form_state, 'inc', 'islandora_object_lock', 'includes/form_handlers');
    islandora_object_lock_acquire_during_alter($form, $form_state, $object, NULL, 'manage/datastreams/add-using-xslt');
    $form_state['object_lock_acquired'] = TRUE;

    if (islandora_object_lock_access_lock($object, 'lock')) {
      $form['source_datastream']['#attributes']['disabled'] = 'disabled';
    }
  }

  $form['actions'] = array(
    '#type' => 'actions',
    'submit' => array(
      '#type' => 'submit',
      '#value' => t('Save output'),
    ),
    'cancel' => array(
      '#title' => t('Cancel'),
      '#type' => 'link',
      '#href' => 'islandora/object/' . $object->id,
    ),
  );

  return $form;
}

/**
 * Validate callback for cwrc_xslt_object_datastream_form().
 */
function cwrc_xslt_object_datastream_form_validate(array $form, array &$form_state) {
  if (empty($form_state['values']['select_xslt'])) {
    form_set_error('source_datastream', t('Please select a valid xslt to do the transformation.'));
  }
}

/**
 * Submit callback for cwrc_xslt_object_datastream_form().
 */
function cwrc_xslt_object_datastream_form_submit(array $form, array &$form_state) {
  $object = $form['#islandora_object'];
  $values = $form_state['values'];
  $source_datastream_id = $values['source_datastream'];
  $xslt_id = $values['select_xslt'];
  $xslt = $form_state['supported_xslts'][$source_datastream_id][$xslt_id];
  $output_datastream_id = $xslt->datastream_id;
  $datastream = isset($object[$output_datastream_id]) ? $object[$output_datastream_id] : $object->constructDatastream($output_datastream_id, 'M');
  $source_xml = $object[$source_datastream_id]->content;

  $saxon_processor = new Saxon\SaxonProcessor();
  $xslt_processor = $saxon_processor->newXsltProcessor();

  $xdm_node = $saxon_processor->parseXmlFromString($source_xml);
  if ($xdm_node) {
    $xslt_processor->setSourceFromXdmValue($xdm_node);
    $xslt_file = drupal_realpath($xslt->file->uri);
    $xslt_processor->compileFromFile($xslt_file);
    $output_xml = $xslt_processor->transformToString();

    $err_count = !$output_xml ? $xslt_processor->getExceptionCount() : 0;
    if ($err_count > 0) {
      $message = t('The XSLT transformer has encountered @count error(s):', ['@count' => $err_count]) . '<ol>';
      for ($i = 0; $i < $err_count; $i++) {
        $err_code = $xslt_processor->getErrorCode($i);
        $err_message = $xslt_processor->getErrorMessage($i);
        $message .= t('<li><b>Error Code:</b> @code<p><b>Error Message</b><br/>@message</p></li>', [
          '@code' => $err_code,
          '@message' => $err_message,
        ]);
      }
      $message .= '</ol>';
      drupal_set_message($message, 'error');
      $xslt_processor->exceptionClear();
    }
    else {
      $datastream->setContentFromString($output_xml);
      if (!isset($object[$output_datastream_id])) {
        // Create the datastream base on the xslt datastream output.
        $datastream->label = $output_datastream_id;
        $datastream->mimetype = $xslt->datastream_mime_type;
        $message = t('The datastream was successfully added!');
      }
      else {
        $message = t('The datastream was successfully updated!');
      }
      $object->ingestDatastream($datastream);
      drupal_set_message($message);
      $form_state['redirect'] = "islandora/object/{$object->id}/manage/datastreams";
    }
    $xslt_processor->clearParameters();
    $xslt_processor->clearProperties();
  }
  else {
    drupal_set_message(t('We were unable to do the transformation please try again later!'), 'error');
  }
}

/**
 * Ajax callback for xslt select ajax callback.
 */
function cwrc_xslt_islandora_add_datastream_xslt_select_ajax_callback($form, $form_state) {
  return $form['select_xslt'];
}

/**
 * Helper function to get the select xslt options.
 */
function _cwrc_xslt_get_options($datastream_id, &$form_state) {
  $xslts = cwrc_xslt_entry_load();
  $xslt_options = array('' => t('- select -'));
  foreach ($xslts as $xslt) {
    $xslt_cmodel_associations = unserialize($xslt->cmodel_associations);
    $is_supported = $xslt_cmodel_associations ? _cwrc_xslt_is_datastream_supported_in_cmodels($xslt_cmodel_associations, $datastream_id) : FALSE;
    if ($is_supported) {
      $file = file_load($xslt->fid);
      $xslt_id = $xslt->xslt_id;
      $xslt_options[$xslt_id] = $file->filename;
      $xslt->file = $file;
      $form_state['supported_xslts'][$datastream_id][$xslt_id] = $xslt;
    }
  }

  return $xslt_options;
}

/**
 * Helper function to check if datastream is supported in cmodels.
 */
function _cwrc_xslt_is_datastream_supported_in_cmodels($cmodels, $datastream_id) {
  $is_supported = FALSE;
  module_load_include('inc', 'islandora', 'includes/utilities');
  foreach ($cmodels as $cmodel_id => $cmodel_label) {
    if ($is_supported) {
      break;
    }
    $cmodel = islandora_object_load($cmodel_id);
    $cmodel_datastreams = islandora_get_datastreams_requirements_from_content_model($cmodel);

    if ($cmodel_datastreams) {
      foreach ($cmodel_datastreams as $datastream) {
        if ($datastream['id'] === $datastream_id) {
          $is_supported = TRUE;
          break;
        }
      }
    }
  }

  return $is_supported;
}

/**
 * Checks whether a datastream is supported or not.
 *
 * @param array $datastream
 *   The datastream.
 *
 * @return bool
 *   True when the datastream is supported false otherwise.
 */
function _cwrc_xslt_datastream_is_supported(array $datastream) {
  $is_supported = !empty($datastream['mimetype']);
  if ($is_supported && is_string($datastream['mimetype'])) {
    $supported_mime_types = array('text/xml', 'application/xml');
    $is_supported = in_array($datastream['mimetype'], $supported_mime_types, TRUE);
  }

  return $is_supported;
}
